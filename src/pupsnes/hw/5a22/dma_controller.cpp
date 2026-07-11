#include "pupsnes/hw/5a22/dma_controller.h"

#include <array>

#include "pupsnes/core/emu_event.h"
#include "pupsnes/core/scheduler.h"
#include "pupsnes/core/signal_event.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/5a22/cpu_mmio.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/memory/systembus.h"

namespace pupsnes {

namespace {

// B-bus offset pattern per DMA mode (fullsnes hardware reference). Each entry
// is the bbad-relative offset applied to the next byte transferred. After
// consuming `length` entries, the index wraps to 0. The byte counter (DAS)
// advances per byte regardless of pattern length, so each "transfer unit"
// implicitly covers `length` decrements of DAS.
struct ModePattern {
  uint8_t length;
  std::array<uint8_t, 4> offsets;
};

constexpr std::array<ModePattern, 8> kModePatterns = {{
    {1, {0, 0, 0, 0}},  // 0: BBAD
    {2, {0, 1, 0, 0}},  // 1: BBAD, BBAD+1
    {2, {0, 0, 0, 0}},  // 2: BBAD, BBAD
    {4, {0, 0, 1, 1}},  // 3: BBAD, BBAD, BBAD+1, BBAD+1
    {4, {0, 1, 2, 3}},  // 4: BBAD, BBAD+1, BBAD+2, BBAD+3
    {4, {0, 1, 0, 1}},  // 5: BBAD, BBAD+1, BBAD, BBAD+1
    {2, {0, 0, 0, 0}},  // 6: alias of mode 2
    {4, {0, 0, 1, 1}},  // 7: alias of mode 3
}};

}  // namespace

DmaController::DmaController(SNES& snes) : Device(snes) {}

void DmaController::MapSystemBus(SystemBus& bus) {
  // Page $43 covers $4300-$43FF (8 channels x 16 bytes). Mirrored across the
  // standard MMIO bank set (banks $00-$3F and $80-$BF). Part of the
  // $4200-$43FF CPU/DMA register block — the 6-master-cycle fast bus class.
  constexpr uint8_t kDmaMmioAccessCycles = 6;
  for (uint8_t bank_base : {uint8_t{0x00U}, uint8_t{0x80U}}) {
    for (uint8_t bank_offset = 0; bank_offset < 0x40U; ++bank_offset) {
      const uint8_t bank = static_cast<uint8_t>(bank_base + bank_offset);
      bus.MapPage({bank, 0x43U, GetDeviceId(), 0x4300U, PageDeviceKind::kSameClockMmio, kDmaMmioAccessCycles, nullptr,
                   nullptr});
    }
  }
}

void DmaController::Reset() {
  channels_.fill({});
  trigger_write_count_ = 0;
  last_trigger_mask_ = 0;

  // HDMA scheduler bootstrap. The first frame starts at master_time 0, so the
  // init signal fires at master_time = kHdmaInitMasterCycles (24). After that,
  // OnHdmaSignal chains itself forward — init -> per-line(V=0..224) -> next
  // frame's init.
  hdma_phase_ = HdmaPhase::kInit;
  hdma_next_v_ = 0;
  hdma_active_mask_ = 0;
  hdma_frame_base_time_ = 0;
  ScheduleNextHdmaInit(0U);
}

void DmaController::ScheduleNextHdmaInit(TimeMasterT frame_base) {
  hdma_phase_ = HdmaPhase::kInit;
  hdma_frame_base_time_ = frame_base;
  snes_->scheduler->ScheduleSignal(frame_base + kHdmaInitMasterCycles, SignalKind::kHdmaFire,
                                   [this](TimeMasterT t) { OnHdmaSignal(t); });
}

void DmaController::OnHdmaSignal(TimeMasterT master_time) {
  if (hdma_phase_ == HdmaPhase::kInit) {
    // Snapshot HDMAEN; mid-frame writes to $420C don't enable new channels
    // (they can only disable via per-line live read). Latched mask drives the
    // rest of the frame.
    hdma_active_mask_ = snes_->cpu_mmio->GetHdmaEn();

    // Per-channel init: copy A1T into A2A, reset NTRL, clear runtime flags.
    for (uint8_t ch = 0; ch < 8U; ++ch) {
      ChannelState& s = channels_[ch];
      if ((hdma_active_mask_ & (1U << ch)) == 0U) {
        s.hdma_do_transfer = false;
        s.hdma_finished = false;
        continue;
      }
      s.a2a = static_cast<uint8_t>(s.a1t & 0xFFU);
      s.a2a_high = static_cast<uint8_t>((s.a1t >> 8U) & 0xFFU);
      s.ntrl = 0U;
      s.hdma_do_transfer = false;
      s.hdma_finished = false;
    }

    // CPU stall: 18 mcyc base + 8 per active channel.
    uint32_t active_count = 0;
    for (uint8_t ch = 0; ch < 8U; ++ch) {
      if ((hdma_active_mask_ & (1U << ch)) != 0U) ++active_count;
    }
    const TimeMasterT end_time = master_time + 18U + 8U * static_cast<TimeMasterT>(active_count);
    if (snes_->GetMasterTime() < end_time) {
      snes_->SetMasterTime(end_time);
    }
    if (snes_->cpu->GetTime() < end_time) {
      snes_->cpu->SetLocalTime(end_time);
    }

    events::EmitWithHv(
        snes_->GetEmuEventSink(), master_time, [&] { return snes_->ppu->ProjectHvAt(master_time); },
        EmuEventKind::kHdmaFrameInit, hdma_active_mask_);

    // Schedule first per-line at V=0 H=274.
    hdma_phase_ = HdmaPhase::kRunLine;
    hdma_next_v_ = 0;
    snes_->scheduler->ScheduleSignal(hdma_frame_base_time_ + kHdmaPerLineMasterCycles, SignalKind::kHdmaFire,
                                     [this](TimeMasterT t) { OnHdmaSignal(t); });
    return;
  }

  // kRunLine: walk active channels and do per-line work. Live HDMAEN gates
  // each channel — mid-frame writes that clear bits stop transfers
  // immediately. Bits set mid-frame are ignored (channel's runtime state
  // wasn't initialized for this frame).
  const uint8_t live_mask = snes_->cpu_mmio->GetHdmaEn();
  const uint8_t effective_mask = static_cast<uint8_t>(hdma_active_mask_ & live_mask);

  TimeMasterT t = master_time;
  uint32_t active_count = 0;
  for (uint8_t ch = 0; ch < 8U; ++ch) {
    if ((effective_mask & (1U << ch)) == 0U) continue;
    if (channels_[ch].hdma_finished) continue;
    ++active_count;
    t = HdmaRunChannelLine(ch, t);
  }
  if (active_count > 0U) {
    t += 8U;  // per-line overhead
    events::EmitWithHv(
        snes_->GetEmuEventSink(), master_time, [&] { return snes_->ppu->ProjectHvAt(master_time); },
        EmuEventKind::kHdmaLineRun, hdma_next_v_, effective_mask, active_count);
  }

  if (snes_->GetMasterTime() < t) {
    snes_->SetMasterTime(t);
  }
  if (snes_->cpu->GetTime() < t) {
    snes_->cpu->SetLocalTime(t);
  }

  // Schedule the next fire. If we just ran V=224 (the last visible line),
  // chain to next frame's init. Otherwise schedule next scanline at +1364.
  if (hdma_next_v_ >= kHdmaLastVisibleV) {
    ScheduleNextHdmaInit(hdma_frame_base_time_ + kHdmaFrameCycles);
  } else {
    ++hdma_next_v_;
    const TimeMasterT next_time = hdma_frame_base_time_ +
                                  static_cast<TimeMasterT>(hdma_next_v_) * kHdmaNormalLineCycles +
                                  kHdmaPerLineMasterCycles;
    snes_->scheduler->ScheduleSignal(next_time, SignalKind::kHdmaFire, [this](TimeMasterT t2) { OnHdmaSignal(t2); });
  }
}

TimeMasterT DmaController::HdmaRunChannelLine(uint8_t ch, TimeMasterT t) {
  ChannelState& s = channels_[ch];
  const uint8_t mode = static_cast<uint8_t>(s.dmap & 0x07U);
  const ModePattern& pat = kModePatterns[mode];

  // Header reload when the lines-remaining counter (low 7 bits) reaches 0.
  // Bit 7 of NTRL is the repeat flag preserved across decrements; the reload
  // overwrites the whole byte with the new entry header.
  if ((s.ntrl & 0x7FU) == 0U) {
    const uint32_t a2a16 = (static_cast<uint32_t>(s.a2a_high) << 8U) | static_cast<uint32_t>(s.a2a);
    const uint32_t header_addr = (static_cast<uint32_t>(s.a1b) << 16U) | a2a16;
    auto rplan = snes_->system_bus->Plan(header_addr, BusAccessType::kRead);
    const uint8_t header = snes_->system_bus->Follow(rplan, t, GetDeviceId()).data;
    t += 8U;

    // A2A++ (16-bit, no carry into the bank — matches fullsnes "bank stays
    // constant" wording for HDMA source-table addressing).
    const uint16_t next_a2a = static_cast<uint16_t>((a2a16 + 1U) & 0xFFFFU);
    s.a2a = static_cast<uint8_t>(next_a2a & 0xFFU);
    s.a2a_high = static_cast<uint8_t>((next_a2a >> 8U) & 0xFFU);

    if (header == 0U) {
      s.hdma_finished = true;
      return t;
    }
    s.ntrl = header;
    s.hdma_do_transfer = true;

    // Indirect mode: after the header byte, the source table holds a 16-bit
    // pointer that becomes the live data address (DAS). Refresh it from the
    // table on each entry reload. The data bank is DASB; the pointer wraps
    // within the bank just like A2A does.
    if ((s.dmap & 0x40U) != 0U) {
      const uint32_t a2a16_lo = (static_cast<uint32_t>(s.a2a_high) << 8U) | static_cast<uint32_t>(s.a2a);
      const uint32_t lo_addr = (static_cast<uint32_t>(s.a1b) << 16U) | a2a16_lo;
      auto lo_plan = snes_->system_bus->Plan(lo_addr, BusAccessType::kRead);
      const uint8_t das_lo = snes_->system_bus->Follow(lo_plan, t, GetDeviceId()).data;
      t += 8U;
      const uint16_t after_lo = static_cast<uint16_t>((a2a16_lo + 1U) & 0xFFFFU);

      const uint32_t hi_addr = (static_cast<uint32_t>(s.a1b) << 16U) | after_lo;
      auto hi_plan = snes_->system_bus->Plan(hi_addr, BusAccessType::kRead);
      const uint8_t das_hi = snes_->system_bus->Follow(hi_plan, t, GetDeviceId()).data;
      t += 8U;
      const uint16_t after_hi = static_cast<uint16_t>((after_lo + 1U) & 0xFFFFU);

      s.das = static_cast<uint16_t>((static_cast<uint16_t>(das_hi) << 8U) | das_lo);
      s.a2a = static_cast<uint8_t>(after_hi & 0xFFU);
      s.a2a_high = static_cast<uint8_t>((after_hi >> 8U) & 0xFFU);
    }
  }

  if (s.hdma_do_transfer) {
    // Mode pattern dictates how many bytes constitute one transfer unit (1,
    // 2, or 4 bytes). Source = indirect ? DASB:DAS : A1B:A2A; advances per
    // byte within its respective bank. Destination = $2100 | (BBAD + offset).
    const bool indirect = (s.dmap & 0x40U) != 0U;
    for (uint8_t i = 0; i < pat.length; ++i) {
      uint32_t src_addr;
      if (indirect) {
        src_addr = (static_cast<uint32_t>(s.dasb) << 16U) | static_cast<uint32_t>(s.das);
      } else {
        const uint32_t a2a16 = (static_cast<uint32_t>(s.a2a_high) << 8U) | static_cast<uint32_t>(s.a2a);
        src_addr = (static_cast<uint32_t>(s.a1b) << 16U) | a2a16;
      }
      const uint32_t dst_addr = 0x2100U | static_cast<uint32_t>(static_cast<uint8_t>(s.bbad + pat.offsets[i]));

      auto rplan = snes_->system_bus->Plan(src_addr, BusAccessType::kRead);
      const uint8_t value = snes_->system_bus->Follow(rplan, t, GetDeviceId()).data;
      auto wplan = snes_->system_bus->Plan(dst_addr, BusAccessType::kWrite, value);
      (void)snes_->system_bus->Follow(wplan, t + 4U, GetDeviceId());
      t += 8U;

      if (indirect) {
        s.das = static_cast<uint16_t>((s.das + 1U) & 0xFFFFU);
      } else {
        const uint32_t a2a16 = (static_cast<uint32_t>(s.a2a_high) << 8U) | static_cast<uint32_t>(s.a2a);
        const uint16_t next_a2a = static_cast<uint16_t>((a2a16 + 1U) & 0xFFFFU);
        s.a2a = static_cast<uint8_t>(next_a2a & 0xFFU);
        s.a2a_high = static_cast<uint8_t>((next_a2a >> 8U) & 0xFFU);
      }
    }
  }

  // Decrement low 7 bits of NTRL; preserve bit 7 (repeat flag).
  const uint8_t repeat_bit = static_cast<uint8_t>(s.ntrl & 0x80U);
  const uint8_t low7 = static_cast<uint8_t>((s.ntrl & 0x7FU) - 1U) & 0x7FU;
  s.ntrl = static_cast<uint8_t>(repeat_bit | low7);
  s.hdma_do_transfer = (repeat_bit != 0U) || (low7 == 0U);

  return t;
}

std::optional<uint8_t> DmaController::ReadRegisterShadow(uint32_t offset) const {
  const uint32_t reg = offset & 0xFFFFU;
  if (reg < 0x4300U || reg >= 0x4380U) {
    return std::nullopt;
  }
  const uint8_t channel = static_cast<uint8_t>((reg >> 4U) & 0x07U);
  const uint8_t local = static_cast<uint8_t>(reg & 0x0FU);
  const ChannelState& ch = channels_[channel];
  switch (local) {
    case 0x0: return ch.dmap;
    case 0x1: return ch.bbad;
    case 0x2: return static_cast<uint8_t>(ch.a1t & 0xFFU);
    case 0x3: return static_cast<uint8_t>((ch.a1t >> 8U) & 0xFFU);
    case 0x4: return ch.a1b;
    case 0x5: return static_cast<uint8_t>(ch.das & 0xFFU);
    case 0x6: return static_cast<uint8_t>((ch.das >> 8U) & 0xFFU);
    case 0x7: return ch.dasb;
    case 0x8: return ch.a2a;
    case 0x9: return ch.a2a_high;
    case 0xA: return ch.ntrl;
    default: return std::nullopt;  // $43xB-$43xF unused per fullsnes.
  }
}

MmioReadResult DmaController::ReadRegister(uint32_t offset, TimeMasterT /*current_time*/) {
  const auto shadow = ReadRegisterShadow(offset);
  if (shadow.has_value()) {
    return {*shadow, 0xFFU};
  }
  return {0x00U, 0x00U};
}

void DmaController::WriteRegister(uint32_t offset, uint8_t data, TimeMasterT /*current_time*/) {
  const uint32_t reg = offset & 0xFFFFU;
  if (reg < 0x4300U || reg >= 0x4380U) {
    // Beyond channel 7 ($4378-$437F is unused per fullsnes). Drop.
    return;
  }
  const uint8_t channel = static_cast<uint8_t>((reg >> 4U) & 0x07U);
  const uint8_t local = static_cast<uint8_t>(reg & 0x0FU);
  ChannelState& ch = channels_[channel];
  switch (local) {
    case 0x0: ch.dmap = data; break;
    case 0x1: ch.bbad = data; break;
    case 0x2: ch.a1t = static_cast<uint16_t>((ch.a1t & 0xFF00U) | data); break;
    case 0x3: ch.a1t = static_cast<uint16_t>((ch.a1t & 0x00FFU) | (static_cast<uint32_t>(data) << 8U)); break;
    case 0x4: ch.a1b = data; break;
    case 0x5: ch.das = static_cast<uint16_t>((ch.das & 0xFF00U) | data); break;
    case 0x6: ch.das = static_cast<uint16_t>((ch.das & 0x00FFU) | (static_cast<uint32_t>(data) << 8U)); break;
    case 0x7: ch.dasb = data; break;
    case 0x8: ch.a2a = data; break;
    case 0x9: ch.a2a_high = data; break;
    case 0xA: ch.ntrl = data; break;
    default:  // $43xB-$43xF unused per fullsnes; drop.
      break;
  }
}

TimeMasterT DmaController::Trigger(uint8_t channels_mask, TimeMasterT start_time) {
  // Snapshot pre-state for the debugger ring before the per-channel loop
  // mutates anything. Always-on; cost is one struct copy per $420B (rare).
  TriggerRecord record;
  record.start_time = start_time;
  record.channels_mask = channels_mask;
  for (uint8_t ch = 0; ch < 8U; ++ch) {
    if ((channels_mask & (1U << ch)) == 0U) continue;
    const ChannelState& s = channels_[ch];
    auto& slot = record.per_channel[ch];
    slot.dmap = s.dmap;
    slot.bbad = s.bbad;
    slot.a1b = s.a1b;
    slot.a1t_start = s.a1t;
    slot.das_start = s.das;
    slot.bytes_transferred = (s.das == 0U) ? 0x10000U : static_cast<uint32_t>(s.das);
  }

  events::EmitWithHv(
      snes_->GetEmuEventSink(), start_time, [&] { return snes_->ppu->ProjectHvAt(start_time); },
      EmuEventKind::kDmaBurstStart, channels_mask);

  TimeMasterT t = start_time + 8U;  // Startup overhead.

  for (uint8_t ch = 0; ch < 8U; ++ch) {
    if ((channels_mask & (1U << ch)) == 0U) continue;
    ChannelState& s = channels_[ch];

    const bool b_to_a = (s.dmap & 0x80U) != 0U;
    const uint8_t step_mode = static_cast<uint8_t>((s.dmap >> 3U) & 0x03U);
    // step_mode: 0=inc, 1=fixed, 2=dec, 3=fixed.
    const int32_t step_delta = (step_mode == 0U) ? 1 : (step_mode == 2U ? -1 : 0);
    const uint8_t mode = static_cast<uint8_t>(s.dmap & 0x07U);
    const ModePattern& pat = kModePatterns[mode];
    uint8_t pat_index = 0;

    // DAS=0 means 64K bytes. Use a do/while loop so the first byte still runs
    // when initial DAS is 0; the post-decrement gets it to 0xFFFF, and the
    // loop continues until the natural zero from the wrap.
    do {
      const uint32_t a_addr = (static_cast<uint32_t>(s.a1b) << 16U) | static_cast<uint32_t>(s.a1t);
      const uint32_t b_addr = static_cast<uint32_t>(0x2100U | static_cast<uint8_t>(s.bbad + pat.offsets[pat_index]));

      if (b_to_a) {
        // B-bus -> A-bus: read from $00:21bb, write to A-bus addr.
        auto rplan = snes_->system_bus->Plan(b_addr, BusAccessType::kRead);
        const auto rresult = snes_->system_bus->Follow(rplan, t, GetDeviceId());
        auto wplan = snes_->system_bus->Plan(a_addr, BusAccessType::kWrite, rresult.data);
        (void)snes_->system_bus->Follow(wplan, t + 4U, GetDeviceId());
      } else {
        // A-bus -> B-bus.
        auto rplan = snes_->system_bus->Plan(a_addr, BusAccessType::kRead);
        const auto rresult = snes_->system_bus->Follow(rplan, t, GetDeviceId());
        auto wplan = snes_->system_bus->Plan(b_addr, BusAccessType::kWrite, rresult.data);
        (void)snes_->system_bus->Follow(wplan, t + 4U, GetDeviceId());
      }

      t += 8U;                                                                  // 8 master cycles per byte.
      s.a1t = static_cast<uint16_t>(static_cast<int32_t>(s.a1t) + step_delta);  // wraps within bank.
      s.das = static_cast<uint16_t>(s.das - 1U);
      pat_index = static_cast<uint8_t>((pat_index + 1U) % pat.length);
    } while (s.das != 0U);
  }

  record.end_time = t;
  trigger_ring_[trigger_write_count_ % kTriggerRingCapacity] = record;
  ++trigger_write_count_;
  last_trigger_mask_ = channels_mask;

  events::EmitWithHv(
      snes_->GetEmuEventSink(), t, [&] { return snes_->ppu->ProjectHvAt(t); }, EmuEventKind::kDmaBurstComplete,
      channels_mask, t - start_time);

  return t;
}

std::size_t DmaController::GetRecentTriggerCount() const {
  return (trigger_write_count_ < kTriggerRingCapacity) ? trigger_write_count_ : kTriggerRingCapacity;
}

const DmaController::TriggerRecord& DmaController::GetRecentTrigger(std::size_t index) const {
  // Oldest-first: when the ring has wrapped, the oldest entry sits at
  // (write_count_ % capacity); otherwise it's at slot 0.
  const std::size_t size = GetRecentTriggerCount();
  const std::size_t start =
      (trigger_write_count_ >= kTriggerRingCapacity) ? (trigger_write_count_ % kTriggerRingCapacity) : 0;
  // Defensive clamp keeps a stray index in-bounds rather than UB.
  const std::size_t clamped = (index < size) ? index : (size > 0 ? size - 1 : 0);
  return trigger_ring_[(start + clamped) % kTriggerRingCapacity];
}

std::optional<uint8_t> DmaController::HandleDebugRead(uint32_t offset) const {
  // Debugger reads of $4300-$437F return the live channel-state shadow.
  return ReadRegisterShadow(offset);
}

bool DmaController::HandleDebugWrite(uint32_t /*offset*/, uint8_t /*data*/) {
  // Accept silently for now; Task 2 wires writes through to the channel shadow.
  return true;
}

}  // namespace pupsnes
