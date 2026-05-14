#include "pupsnes/hw/5a22/cpu_mmio.h"

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/cartridge.h"
#include "pupsnes/hw/dma_controller.h"
#include "pupsnes/hw/joypad.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/signal_event.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/hw/sppu/ppu_regs.h"
#include "pupsnes/hw/systembus.h"

namespace pupsnes {

namespace {

// Pages $40-$43 cover both the legacy joypad serial ports ($4016/$4017 on
// page $40) and the standard 5A22 MMIO block ($4200-$43FF). Mapping the whole
// span keeps stray reads in this region from falling through to "unmapped" on
// the bus log even though most addresses inside $40-$41 are unimplemented.
//
// Access timing splits the span in two:
//   * Pages $40-$41 ($4000-$41FF) — manual joypad area. Each access costs 12
//     master cycles on real hardware (slowest bus class). The only live regs
//     here are $4016/$4017; everything else on these pages is unmapped but
//     still charged the 12-cycle slot.
//   * Pages $42-$43 ($4200-$43FF) — CPU/DMA register block. Real hardware
//     bills 6 master cycles, but PupSNES currently charges 8 across the
//     board for the wider MMIO span; see the TODO for the follow-up that
//     drops $20-$21 and $42-$43 to 6.
constexpr uint8_t kFirstMmioPage = 0x40U;
constexpr uint8_t kLastMmioPage = 0x43U;
constexpr uint8_t kFirstJoypadPage = 0x40U;
constexpr uint8_t kLastJoypadPage = 0x41U;
constexpr uint8_t kJoypadAccessCycles = 12U;
constexpr uint8_t kMmioAccessCycles = 8U;

void MapMmioBank(SystemBus& bus, DeviceIdT device_id, uint8_t bank) {
  for (uint16_t page = kFirstMmioPage; page <= kLastMmioPage; ++page) {
    const uint32_t base_offset = static_cast<uint32_t>(page) * 0x100U;
    const uint8_t access_cycles =
        (page >= kFirstJoypadPage && page <= kLastJoypadPage) ? kJoypadAccessCycles : kMmioAccessCycles;
    bus.MapPage({bank, static_cast<uint8_t>(page), device_id, base_offset, PageDeviceKind::kSameClockMmio,
                 access_cycles, nullptr, nullptr});
  }
}

}  // namespace

CpuMmio::CpuMmio(SNES* snes) : Device(snes) {}

void CpuMmio::Reset() {
  const bool was_fast = (memsel_ & 0x01U) != 0U;
  memsel_ = 0;
  nmitimen_ = 0;
  htime_ = 0;
  vtime_ = 0;
  timeup_latch_ = false;
  scheduled_match_time_ = kNoMatch;
  // Mirror the write path: only rebuild the page table when FASTROM was
  // actually on. No-op remap on a cold machine where both state and bus
  // already agree.
  if (was_fast && snes_ != nullptr && snes_->cartridge != nullptr && snes_->system_bus != nullptr) {
    snes_->cartridge->OnMemSelChanged(*snes_->system_bus, false);
  }
}

void CpuMmio::MapSystemBus(SystemBus& bus) {
  for (uint8_t bank_base : {uint8_t{0x00U}, uint8_t{0x80U}}) {
    for (uint8_t bank_offset = 0; bank_offset < 0x40U; ++bank_offset) {
      MapMmioBank(bus, GetDeviceId(), static_cast<uint8_t>(bank_base + bank_offset));
    }
  }
}

MmioReadResult CpuMmio::ReadRegister(uint32_t offset, TimeMasterT current_time) {
  const uint32_t reg = offset & 0xFFFFU;
  if (reg == kMemSelOffset) {
    return {memsel_, 0xFFU};
  }
  if (reg == kRdNmiOffset) {
    // RDNMI read both samples and clears the VBlank NMI latch. Polling loops
    // of the form `BIT $4210 / BPL` see bit 7 high once per frame at VBlank
    // entry and fall back to 0 on the next read until the latch re-arms.
    bool vblank_nmi = false;
    if (snes_ != nullptr && snes_->ppu != nullptr) {
      vblank_nmi = snes_->ppu->QueryAndClearVblankNmiFlag(current_time);
    }
    uint8_t value = kRdNmiCpuVersion;
    if (vblank_nmi) value = static_cast<uint8_t>(value | kRdNmiVblankFlagMask);
    return {value, kRdNmiDrivenMask};
  }
  if (reg == kTimeUpOffset) {
    // TIMEUP read: bit 7 reports the latch; reading clears it and de-asserts
    // /IRQ. Bits 6:0 are open-bus on hardware.
    const uint8_t value = timeup_latch_ ? kTimeUpFlagMask : uint8_t{0};
    timeup_latch_ = false;
    return {value, kTimeUpFlagMask};
  }
  if (reg == kHvbJoyOffset) {
    // Query the PPU directly so the flags reflect the bus cycle's master time.
    // The PPU catches up internally; missing the call would leak stale h/v
    // from whenever the PPU last advanced.
    PpuHvbStatus status{false, false};
    if (snes_ != nullptr && snes_->ppu != nullptr) {
      status = snes_->ppu->QueryHvbStatus(current_time);
    }
    uint8_t value = 0;
    if (status.vblank) value = static_cast<uint8_t>(value | kHvbJoyVblankMask);
    if (status.hblank) value = static_cast<uint8_t>(value | kHvbJoyHblankMask);
    // Auto-joypad busy bit stays 0 until the joypad auto-read controller lands.
    return {value, kHvbJoyDrivenMask};
  }
  if (reg == kJoySer0Offset) {
    // Manual serial port for P1. Only bit 0 carries pad data; leave bits 7-1
    // as open-bus rather than fabricating zeros — real hardware exposes a few
    // open I/O pins in that window.
    if (snes_ != nullptr && snes_->joypad != nullptr) {
      return {snes_->joypad->ReadJoySer0(), 0x01U};
    }
    return {0x00U, 0x01U};
  }
  if (reg == kJoySer1Offset) {
    // P2 manual serial port. No P2 controller, so the data line reads 0 with
    // bit 0 driven; bits 7-1 stay open-bus.
    if (snes_ != nullptr && snes_->joypad != nullptr) {
      return {snes_->joypad->ReadJoySer1(), 0x01U};
    }
    return {0x00U, 0x01U};
  }
  if (reg == kAutoJoyResultFirst) {
    // $4218 JOY1L — A, X, L, R in bits 7..4, controller-type ID in bits 3..0.
    const uint8_t value = (snes_ != nullptr && snes_->joypad != nullptr) ? snes_->joypad->ReadJoy1L() : 0x00U;
    return {value, 0xFFU};
  }
  if (reg == kAutoJoyResultFirst + 1U) {
    // $4219 JOY1H — B, Y, Select, Start, Up, Down, Left, Right.
    const uint8_t value = (snes_ != nullptr && snes_->joypad != nullptr) ? snes_->joypad->ReadJoy1H() : 0x00U;
    return {value, 0xFFU};
  }
  if (reg > kAutoJoyResultFirst + 1U && reg <= kAutoJoyResultLast) {
    // $421A-$421F: JOY2/JOY3/JOY4. No P2-P4 controllers; drive zero so polling
    // doesn't pick up open-bus garbage as phantom button presses.
    return {0x00U, 0xFFU};
  }
  // Stub: other CPU MMIO registers (NMITIMEN, RDNMI, HDMA) are not yet
  // modeled. Return pure open-bus (mask=0) so the bus merges in the last data
  // value instead of a hard zero.
  return {0x00U, 0x00U};
}

void CpuMmio::WriteRegister(uint32_t offset, uint8_t data, TimeMasterT current_time) {
  const uint32_t reg = offset & 0xFFFFU;
  if (reg == kMemSelOffset) {
    const bool was_fast = (memsel_ & 0x01U) != 0U;
    memsel_ = data;
    const bool now_fast = (memsel_ & 0x01U) != 0U;
    // Re-map the LoROM fast-bank pages only when the FASTROM bit actually
    // flipped. ROMs commonly re-poke $420D with the same value; avoid
    // reshuffling 126 banks * 128 pages of page-table entries on no-op writes.
    if (was_fast != now_fast && snes_ != nullptr && snes_->cartridge != nullptr && snes_->system_bus != nullptr) {
      snes_->cartridge->OnMemSelChanged(*snes_->system_bus, now_fast);
    }
    return;
  }
  if (reg == kNmiTimenOffset) {
    const uint8_t prev = nmitimen_;
    nmitimen_ = data;
    // NMI-enable transitions trigger the two NMITIMEN.7 quirks (0→1
    // transparency while /NMI is asserted → immediate NMI; 1→0 cancels any
    // pending NMI). The CPU owns the flip-flop and runs the transition
    // logic, since it's the one with the edge-tracker state.
    if (((prev ^ data) & kNmiTimenNmiEnableMask) != 0U && snes_ != nullptr && snes_->cpu != nullptr) {
      snes_->cpu->OnNmiTimenChanged(prev, data, current_time);
    }
    // H/V-IRQ mode transitions: 5:4 = 00 clears any pending latch (per
    // fullsnes "writing $4200 with bits 5/4=0 cancels the IRQ"), and
    // re-arming a different mode reschedules from the current cycle.
    const uint8_t prev_mode = static_cast<uint8_t>(prev & kNmiTimenIrqModeMask);
    const uint8_t new_mode = static_cast<uint8_t>(data & kNmiTimenIrqModeMask);
    if (prev_mode != new_mode) {
      if (new_mode == 0U) {
        // Cancellation: drop the latch and the pending scheduler signal.
        timeup_latch_ = false;
        scheduled_match_time_ = kNoMatch;
      } else {
        RescheduleIrqMatchFrom(current_time);
      }
    }
    return;
  }
  if (reg == kHTimeLOffset || reg == kHTimeHOffset || reg == kVTimeLOffset || reg == kVTimeHOffset) {
    if (reg == kHTimeLOffset) {
      htime_ = static_cast<uint16_t>((htime_ & 0x0100U) | data);
    } else if (reg == kHTimeHOffset) {
      const uint16_t high_bit = static_cast<uint16_t>((data & 0x01U) << 8U);
      htime_ = static_cast<uint16_t>((htime_ & 0x00FFU) | high_bit);
    } else if (reg == kVTimeLOffset) {
      vtime_ = static_cast<uint16_t>((vtime_ & 0x0100U) | data);
    } else {
      const uint16_t high_bit = static_cast<uint16_t>((data & 0x01U) << 8U);
      vtime_ = static_cast<uint16_t>((vtime_ & 0x00FFU) | high_bit);
    }
    // Re-arming on every write — even when the IRQ mode is 00 the cost is
    // tiny (early-out inside the helper). Updating mid-frame causes the
    // already-queued event to be replaced by one at the new target.
    if ((nmitimen_ & kNmiTimenIrqModeMask) != 0U) {
      RescheduleIrqMatchFrom(current_time);
    }
    return;
  }
  if (reg == kMdmaEnOffset) {
    if (data != 0U && snes_ != nullptr && snes_->dma != nullptr) {
      const TimeMasterT end_time = snes_->dma->Trigger(data, current_time);
      // Stall the CPU: bump master time and the CPU's local-time mirror so the
      // next bus access sees the post-DMA cycle. Real hardware halts the 65816
      // for the DMA duration; this models the same effect inline.
      snes_->SetMasterTime(end_time);
      if (snes_->cpu != nullptr) {
        snes_->cpu->SetLocalTime(end_time);
      }
    }
    return;
  }
  if (reg == kHdmaEnOffset) {
    // HDMA enable: shadow only in v1 (HDMA itself not yet implemented).
    hdmaen_ = data;
    return;
  }
  if (reg == kJoySer0Offset) {
    // $4016 write — bit 0 is the manual-serial strobe for both controller
    // ports. Bits 7-1 are programmable I/O pins on the controller connector;
    // ignored here because we don't model the I/O port.
    if (snes_ != nullptr && snes_->joypad != nullptr) {
      snes_->joypad->WriteJoySer0(data);
    }
    return;
  }
  // Stub: writes to other registers are accepted silently so ROMs can poke
  // them without the bus rejecting the transaction.
}

std::optional<uint8_t> CpuMmio::HandleDebugRead(uint32_t offset) const {
  const uint32_t reg = offset & 0xFFFFU;
  if (reg == kMemSelOffset) {
    return memsel_;
  }
  if (reg == kNmiTimenOffset) {
    // $4200 is write-only on real hardware; expose the shadow for the debugger.
    return nmitimen_;
  }
  if (reg == kHTimeLOffset) {
    return static_cast<uint8_t>(htime_ & 0xFFU);
  }
  if (reg == kHTimeHOffset) {
    return static_cast<uint8_t>((htime_ >> 8U) & 0x01U);
  }
  if (reg == kVTimeLOffset) {
    return static_cast<uint8_t>(vtime_ & 0xFFU);
  }
  if (reg == kVTimeHOffset) {
    return static_cast<uint8_t>((vtime_ >> 8U) & 0x01U);
  }
  return 0x00U;
}

bool CpuMmio::HandleDebugWrite(uint32_t offset, uint8_t data) {
  const uint32_t reg = offset & 0xFFFFU;
  if (reg == kMemSelOffset || reg == kNmiTimenOffset || reg == kHTimeLOffset || reg == kHTimeHOffset ||
      reg == kVTimeLOffset || reg == kVTimeHOffset) {
    // Debug writes are out-of-band and don't belong to a bus cycle; pass 0
    // as the current time. CpuMmio commits synchronously, so the timestamp
    // is unused. Devices that use lazy replay (PPU) must not be debug-written
    // through this path — they provide their own HandleDebugWrite override.
    WriteRegister(reg, data, 0);
  }
  return true;
}

void CpuMmio::HandleIrqMatch(TimeMasterT t) {
  // Fence-fired at the master cycle a programmed (H, V) match landed on.
  // MachineSync has already advanced every device to t, so it's safe to
  // latch + re-arm without worrying about stale PPU state.
  scheduled_match_time_ = kNoMatch;
  if ((nmitimen_ & kNmiTimenIrqModeMask) == 0U) {
    return;  // mode was cleared between scheduling and firing.
  }
  timeup_latch_ = true;
  RescheduleIrqMatchFrom(t);
}

namespace {

// Compute the next master cycle >= `now` at which the configured trigger mode
// fires. `now` is the master cycle the caller is asking from. Returns
// CpuMmio::kNoMatch when no match is reachable (e.g., VTIME out of range).
//
// Mode bits per NMITIMEN: 01 = V only, 10 = H only, 11 = both.
constexpr TimeMasterT kMatchNoMatch = static_cast<TimeMasterT>(-1);

TimeMasterT ComputeNextMatch(uint8_t irq_mode, uint16_t htime, uint16_t vtime, TimeMasterT now, const Ppu& ppu) {
  // We anchor frames at master cycle 0 (the PPU reset point). All NTSC frames
  // up to the first short-line frame are 1364×262 = 357,368 mcyc long; the
  // short-line saving (V=240 on field==true) takes 4 mcyc off every other
  // frame. We model this by walking frame origins forward from 0 using the
  // field cadence — `field_` toggles at end-of-frame and `field_==true`
  // means the *next* frame's V=240 is short. The PPU's reset puts field=false
  // for frame 0, so frames alternate F=0 (1364×262 = 357368) and
  // F=1 (357364) starting from frame 1.
  //
  // For correctness we don't try to be clever: walk frame-by-frame until the
  // candidate match cycle lands at or after `now`. With a 60Hz frame rate this
  // is at most a single iteration in the common case (write-during-frame → fire
  // later in same frame, or next frame at the latest).
  const bool h_enabled = (irq_mode & CpuMmio::kNmiTimenHIrqEnableMask) != 0U;
  const bool v_enabled = (irq_mode & CpuMmio::kNmiTimenVIrqEnableMask) != 0U;
  const uint32_t target_h = h_enabled ? static_cast<uint32_t>(htime) : 0U;

  // VTIME out-of-range when V-mode is engaged: no match this frame, but the V
  // counter wraps each frame so the next frame will still skip. Treat as "no
  // match" — game programmer error.
  if (v_enabled && vtime >= sppu::regs::kLinesPerFrameNtsc) {
    return kMatchNoMatch;
  }
  // HTIME out-of-range similarly.
  if (h_enabled && target_h >= sppu::regs::kDotsPerLine) {
    return kMatchNoMatch;
  }

  Ppu::Cursor cursor = ppu.GetCursor();
  // Frame origin = master cycle at which (h=0, v=0) of the cursor's frame
  // entered. Cursor's local_time is the most recently caught-up master cycle;
  // subtract the cycles into the frame so we land on the frame boundary.
  TimeMasterT frame_origin = cursor.local_time;
  {
    // Subtract the master cycles inside the current frame so far.
    TimeMasterT inside = 0;
    for (uint32_t v = 0; v < cursor.v; ++v) {
      inside += Ppu::LineCycles(v, cursor.field);
    }
    for (uint32_t h = 0; h < cursor.h; ++h) {
      inside += Ppu::DotCost(h, cursor.v, cursor.field);
    }
    frame_origin -= inside;
  }
  // When the bus write happens before any PPU catch-up, GetCursor() may report
  // (h=0, v=0) at master_time=0 — i.e., frame_origin = 0 and cursor.field =
  // false. Either way `frame_origin` is the absolute master cycle of the
  // frame the cursor sits inside.

  bool field = cursor.field;
  // V-only mode means "match at (h=0, V=VTIME) of every frame"; H-only mode
  // means "match at (H=HTIME, every V)". Both means "(H, V) once per frame".
  if (h_enabled && !v_enabled) {
    // H-only: walk forward scanline-by-scanline.
    uint32_t v = cursor.v;
    while (true) {
      const TimeMasterT cand = Ppu::MasterCycleAt(target_h, v, frame_origin, field);
      if (cand > now) {
        return cand;
      }
      ++v;
      if (v >= sppu::regs::kLinesPerFrameNtsc) {
        frame_origin += Ppu::LineCycles(sppu::regs::kLinesPerFrameNtsc - 1U, field) * 0U;  // placeholder
        // Compute full frame length and roll over.
        TimeMasterT frame_len = 0;
        for (uint32_t vv = 0; vv < sppu::regs::kLinesPerFrameNtsc; ++vv) {
          frame_len += Ppu::LineCycles(vv, field);
        }
        // Reset frame_origin by adding frame length; PPU's actual frame origin
        // is independent but the cursor walks linearly.
        frame_origin = Ppu::MasterCycleAt(0U, 0U, frame_origin, field) + frame_len;
        field = !field;
        v = 0;
      }
    }
  }

  // V-only (mode 01) or both (mode 11): match once per frame at (target_h,
  // target_v). For mode 01, target_h is 0 (start of line). For mode 11, both
  // h_enabled and v_enabled are true. Walk frames forward.
  while (true) {
    const TimeMasterT cand = Ppu::MasterCycleAt(target_h, vtime, frame_origin, field);
    if (cand > now) {
      return cand;
    }
    // Roll to next frame.
    TimeMasterT frame_len = 0;
    for (uint32_t vv = 0; vv < sppu::regs::kLinesPerFrameNtsc; ++vv) {
      frame_len += Ppu::LineCycles(vv, field);
    }
    frame_origin += frame_len;
    field = !field;
  }
}

}  // namespace

void CpuMmio::RescheduleIrqMatchFrom(TimeMasterT now) {
  if (snes_ == nullptr || snes_->scheduler == nullptr || snes_->ppu == nullptr) {
    return;
  }
  const uint8_t mode = static_cast<uint8_t>(nmitimen_ & kNmiTimenIrqModeMask);
  if (mode == 0U) {
    scheduled_match_time_ = kNoMatch;
    return;
  }
  const TimeMasterT t = ComputeNextMatch(mode, htime_, vtime_, now, *snes_->ppu);
  if (t == kMatchNoMatch) {
    scheduled_match_time_ = kNoMatch;
    return;
  }
  scheduled_match_time_ = t;
  // Note: a previously scheduled kHIrqMatch handler may still fire at its old
  // time; HandleIrqMatch compares against scheduled_match_time_ and treats a
  // mismatch as stale (the queue has no cancel API). The handler captures the
  // expected time so re-scheduling correctly supersedes the prior event.
  const TimeMasterT expected = scheduled_match_time_;
  snes_->scheduler->ScheduleSignal(t, SignalKind::kHIrqMatch, [this, expected](TimeMasterT fire_time) {
    // Drop stale events: a write to the timer regs after we were
    // scheduled supersedes us.
    if (scheduled_match_time_ != expected) return;
    HandleIrqMatch(fire_time);
  });
}

}  // namespace pupsnes
