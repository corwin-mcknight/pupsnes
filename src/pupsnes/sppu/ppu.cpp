#include "pupsnes/hw/sppu/ppu.h"

#include <algorithm>
#include <memory>

#include "pupsnes/hw/apu_stub.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/signal_event.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"

namespace pupsnes {

namespace {

// Pack two VRAM bytes (low, high) into a 16-bit word for the prefetch buffer.
constexpr uint16_t PackWord(uint8_t lo, uint8_t hi) {
  return static_cast<uint16_t>(static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8));
}

// Wrap an OAM byte address into its physical slot. OAM is 544 bytes: 512
// low-table + 32 high-table starting at $200. Byte addresses in $220-$3FF
// mirror back into the 32-byte high table.
constexpr uint16_t OamByteSlot(uint16_t byte_addr) {
  const uint16_t addr = byte_addr & 0x3FFU;
  if (addr < 0x200U) {
    return addr;
  }
  return static_cast<uint16_t>(0x200U | (addr & 0x1FU));
}

constexpr std::array<uint8_t, 4> kTmMaskForBg = {sppu::regs::kTmBg1Mask, sppu::regs::kTmBg2Mask,
                                                 sppu::regs::kTmBg3Mask, sppu::regs::kTmBg4Mask};

}  // namespace

Ppu::Ppu(SNES* snes)
    : Device(snes),
      vram_(std::make_unique<std::array<uint8_t, sppu::regs::kVramSize>>()),
      oam_(std::make_unique<std::array<uint8_t, sppu::regs::kOamSize>>()),
      cgram_(std::make_unique<std::array<uint16_t, sppu::regs::kCgramWords>>()),
      front_buffer_(std::make_unique<std::array<uint16_t, sppu::regs::kFrameBufferPixels>>()),
      back_buffer_(std::make_unique<std::array<uint16_t, sppu::regs::kFrameBufferPixels>>()),
      drawn_mask_(std::make_unique<std::array<uint8_t, (sppu::regs::kFrameBufferPixels + 7U) / 8U>>()),
      pending_writes_(std::make_unique<std::array<PpuPokeLogEntry, kPendingWriteLogSize>>()) {}

void Ppu::MapSystemBus(SystemBus& bus) {
  // Page $21 covers the B-bus PPU window ($2100-$21FF). The PPU owns the
  // whole page in v1 — offsets $2100-$213F dispatch to real registers, and
  // $2140-$21FF surfaces as open-bus / dropped writes from ReadRegister /
  // WriteRegister below. The APU and WRAM ports move in here later.
  for (uint8_t bank_base : {uint8_t{0x00U}, uint8_t{0x80U}}) {
    for (uint8_t bank_offset = 0; bank_offset < 0x40U; ++bank_offset) {
      const uint8_t bank = static_cast<uint8_t>(bank_base + bank_offset);
      bus.MapPage({bank, 0x21U, GetDeviceId(), 0x2100U, PageDeviceKind::kSameClockMmio, 8, nullptr, nullptr});
    }
  }
}

void Ppu::Reset() {
  shadow_.fill(0);
  // INIDISP power-on: forced blank set, brightness 0. ROMs disable forced
  // blank and ramp brightness in their init code.
  forced_blank_ = true;
  brightness_ = 0;
  overscan_ = false;

  bg_mode_ = 0;
  bg3_priority_ = false;
  bg_tile_16x16_.fill(false);
  bg_tilemap_word_base_.fill(0);
  bg_tilemap_layout_.fill(0);
  bg_char_word_base_.fill(0);
  bg_hofs_.fill(0);
  bg_vofs_.fill(0);
  bg_scroll_prev_ = 0;
  main_screen_layers_ = 0;

  obj_size_select_ = 0;
  obj_region0_word_ = 0;
  obj_region1_word_ = 0;

  cgadd_ = 0;
  cgram_write_latch_data_ = 0;
  cgram_write_latch_high_ = false;
  cgram_read_latch_data_ = 0;
  cgram_read_latch_high_ = false;

  vmadd_ = 0;
  vmain_ = 0;
  vram_prefetch_ = 0;

  oam_byte_addr_ = 0;
  oam_byte_addr_reload_ = 0;
  oam_priority_rotation_ = false;
  oam_write_latch_ = 0;

  h_ = 0;
  v_ = 0;
  field_ = false;

  pending_writes_count_ = 0;
  pending_writes_cursor_ = 0;
  partial_dot_cycles_ = 0;

  vblank_nmi_flag_ = false;

  vram_->fill(0);
  oam_->fill(0);
  cgram_->fill(0);
  front_buffer_->fill(0);
  back_buffer_->fill(0);
  drawn_mask_->fill(0);

  // Schedule the first frame-end signal. CatchUpTo drives all dot emission;
  // the signal fires when the frame boundary arrives so OnFrameEndSignal can
  // chain the next frame's signal.
  if (snes_ != nullptr && snes_->scheduler != nullptr) {
    const TimeMasterT frame_mcyc = 262U * sppu::regs::kNormalLineCycles - (field_ ? 4U : 0U);
    snes_->scheduler->ScheduleSignal(frame_mcyc, SignalKind::kFrameEnd, [this](TimeMasterT t) { OnFrameEndSignal(t); });
    // VBlank-NMI boundary: the CPU's NMI flip-flop is edge-triggered on the
    // /NMI line's falling edge, which lands when V transitions onto the
    // VBlank entry line (225 normally, 240 with SETINI overscan — SETINI
    // starts clear at reset so V=225). Scheduling this as a scheduler signal
    // gives a sync fence: the CPU cannot run past the assertion cycle in a
    // single tick budget, which is the only way to guarantee it can't
    // "time-travel over" an NMI that real hardware would have delivered.
    const uint32_t vblank_start = overscan_ ? sppu::regs::kVisibleVEnd239 : sppu::regs::kVisibleVEnd224;
    const TimeMasterT nmi_boundary_mcyc = static_cast<TimeMasterT>(vblank_start) * sppu::regs::kNormalLineCycles;
    snes_->scheduler->ScheduleSignal(nmi_boundary_mcyc, SignalKind::kVblankNmiBoundary,
                                     [this](TimeMasterT t) { OnVblankNmiBoundarySignal(t); });
  }
}

void Ppu::CatchUpTo(TimeMasterT target) {
  if (target <= local_time_) {
    return;  // idempotent
  }
  while (local_time_ < target) {
    const TimeMasterDeltaT dot_cost = DotCost(h_, v_, field_);
    const TimeMasterDeltaT remaining_dot = dot_cost - partial_dot_cycles_;
    const TimeMasterDeltaT available = target - local_time_;

    if (available < remaining_dot) {
      // Target lands mid-dot. Bank the partial progress without emitting yet.
      partial_dot_cycles_ += available;
      local_time_ += available;
      return;
    }

    // Enough time to finish this dot. Drain writes up to the dot's nominal
    // start cycle so EmitPixel observes the state valid at dot-start.
    const TimeMasterT dot_start_time = local_time_ - static_cast<TimeMasterDeltaT>(partial_dot_cycles_);
    DrainPendingWritesUpTo(dot_start_time);
    EmitPixel(h_, v_);

    // Track that this pixel has been drawn in the current frame.
    const uint32_t idx = FramebufferIndexFor(h_, v_);
    if (idx < sppu::regs::kFrameBufferPixels) {
      (*drawn_mask_)[idx >> 3U] |= static_cast<uint8_t>(1U << (idx & 7U));
    }

    AdvanceHv();
    local_time_ += remaining_dot;
    partial_dot_cycles_ = 0;

    if (h_ == 0 && v_ == 0) {
      // Frame boundary: swap buffers, fire frontend callback, toggle field,
      // clear drawn mask for the new frame.
      OnEndOfFrame();
      field_ = !field_;
      drawn_mask_->fill(0);
    }
  }
}

bool Ppu::QueryAndClearVblankNmiFlag(TimeMasterT current_time) {
  CatchUpTo(current_time);
  const bool was_set = vblank_nmi_flag_;
  vblank_nmi_flag_ = false;
  return was_set;
}

bool Ppu::SampleNmiLine(TimeMasterT current_time) {
  CatchUpTo(current_time);
  return PeekNmiLine();
}

bool Ppu::PeekNmiLine() const {
  // The PPU /NMI output pin asserts for the duration of the VBlank entry line
  // (V == 225 normally, V == 240 with SETINI overscan). It releases as V
  // advances past that line. See fullsnes §PPU NMI.
  const uint32_t vblank_start = overscan_ ? sppu::regs::kVisibleVEnd239 : sppu::regs::kVisibleVEnd224;
  return v_ == vblank_start;
}

void Ppu::OnVblankNmiBoundarySignal(TimeMasterT master_time) {
  // One frame period between consecutive /NMI falling edges. Frame length is
  // kLinesPerFrameNtsc × kNormalLineCycles master cycles, minus 4 when the
  // short line at V=240 with field_==true lies within that frame. At boundary
  // firing, field_ is the current frame's field (it toggles at V=0 of the
  // next frame, AFTER V=240), so the short-line saving applies regardless of
  // whether the threshold is V=225 (V=240 still ahead in the same frame) or
  // V=240 (the boundary IS the short line's start).
  //
  // Only the falling edge needs a fence. The rising edge doesn't latch
  // anything new — the CPU's NMI flip-flop was already set on the falling
  // edge and persists until interrupt acknowledge or NMITIMEN.7 clear.
  // $4210's line-end latch clear is observed lazily via read-triggered
  // catch-up, so one signal per frame suffices.
  //
  // SETINI overscan mid-frame toggle is not compensated: if overscan flipped
  // between scheduling and firing, the fence is one frame out of phase with
  // the true threshold until the next AdvanceHv/reschedule realigns. The
  // CPU's lazy-pull line check still sees the real assertion whenever the
  // PPU is caught up past the new threshold, so correctness holds — only
  // the sync-granularity is coarser in that edge case.
  TimeMasterT period = static_cast<TimeMasterT>(sppu::regs::kLinesPerFrameNtsc) * sppu::regs::kNormalLineCycles;
  if (field_) period -= 4U;
  const TimeMasterT next_boundary = master_time + period;
  if (snes_ != nullptr && snes_->scheduler != nullptr) {
    snes_->scheduler->ScheduleSignal(next_boundary, SignalKind::kVblankNmiBoundary,
                                     [this](TimeMasterT t) { OnVblankNmiBoundarySignal(t); });
  }
}

PpuHvbStatus Ppu::QueryHvbStatus(TimeMasterT current_time) {
  CatchUpTo(current_time);
  // h_/v_ point at the next dot to emit after catch-up. VBlank start tracks
  // SETINI overscan live — fullsnes notes the bit can flip mid-frame; v1
  // scaffold follows the current overscan_ value rather than a per-frame
  // latch.
  const uint32_t vblank_start = overscan_ ? sppu::regs::kVisibleVEnd239 : sppu::regs::kVisibleVEnd224;
  const bool vblank = v_ >= vblank_start;
  // HBlank canonical boundary: H>=274 (fullsnes). Narrower than the
  // "outside visible window" definition by 4 dots, matching how games that
  // poll HVBJOY bit 6 expect the edge to land.
  const bool hblank = h_ >= 274U;
  return {vblank, hblank};
}

void Ppu::OnFrameEndSignal(TimeMasterT master_time) {
  // MachineSync has already called our CatchUpTo(master_time), so the frame
  // boundary (h==0, v==0) has already triggered OnEndOfFrame + buffer swap.
  // Schedule the next frame's boundary signal.
  const TimeMasterT next_frame_mcyc = master_time + 262U * sppu::regs::kNormalLineCycles - (field_ ? 4U : 0U);
  snes_->scheduler->ScheduleSignal(next_frame_mcyc, SignalKind::kFrameEnd,
                                   [this](TimeMasterT t) { OnFrameEndSignal(t); });
}

MmioReadResult Ppu::ReadRegister(uint32_t offset, TimeMasterT current_time) {
  // Lazy-replay contract: any write with cycle <= current_time must be
  // visible to this read. The scheduler's same-clock catch-up drains the
  // pending log during Tick, but Tick overshoots budget by up to 5 cycles
  // (a dot is atomic), leaving the PPU slightly ahead of the bus. When the
  // PPU is ahead, Scheduler::CatchUpDevice is a no-op — so we drain here
  // directly to guarantee the read observes every qualifying write.
  DrainPendingWritesUpTo(current_time);

  const uint16_t reg = static_cast<uint16_t>(offset & 0xFFFFU);
  if (reg < sppu::regs::kBase || reg >= sppu::regs::kEnd) {
    if (reg >= ApuStub::kPortBase && reg < ApuStub::kPortEnd && snes_ != nullptr && snes_->apu_stub != nullptr) {
      return snes_->apu_stub->ReadRegister(reg, current_time);
    }
    // $2180-$21FF WRAM ports still open-bus until that device lands.
    return {0x00U, 0x00U};
  }

  switch (reg) {
    case sppu::regs::kRdOam: {
      const uint8_t value = ReadOamByte(oam_byte_addr_);
      oam_byte_addr_ = static_cast<uint16_t>((oam_byte_addr_ + 1U) & 0x3FFU);
      return {value, 0xFFU};
    }
    case sppu::regs::kRdVramL: {
      const uint8_t value = static_cast<uint8_t>(vram_prefetch_ & 0xFFU);
      if ((vmain_ & sppu::regs::kVmainIncrementOnHighMask) == 0U) {
        PrefetchVram();
        vmadd_ = static_cast<uint16_t>(vmadd_ + VmainIncrementStep());
      }
      return {value, 0xFFU};
    }
    case sppu::regs::kRdVramH: {
      const uint8_t value = static_cast<uint8_t>((vram_prefetch_ >> 8) & 0xFFU);
      if ((vmain_ & sppu::regs::kVmainIncrementOnHighMask) != 0U) {
        PrefetchVram();
        vmadd_ = static_cast<uint16_t>(vmadd_ + VmainIncrementStep());
      }
      return {value, 0xFFU};
    }
    case sppu::regs::kRdCgram: {
      const uint16_t word = (*cgram_)[cgadd_];
      if (!cgram_read_latch_high_) {
        cgram_read_latch_high_ = true;
        return {static_cast<uint8_t>(word & 0xFFU), 0xFFU};
      }
      cgram_read_latch_high_ = false;
      const uint8_t high_byte = static_cast<uint8_t>((word >> 8) & 0x7FU);
      cgadd_ = static_cast<uint8_t>(cgadd_ + 1U);
      // Bit 7 of CGRAM high-byte reads is open-bus — only bits 6:0 are
      // driven by the PPU. The bus merges the floating bit from the latch.
      return {high_byte, 0x7FU};
    }
    case sppu::regs::kStat77: {
      // Bits 3:0 = PPU1 version (1), bits 6:4 open-bus, bit 7 time-over
      // (stubbed 0). Only the driven bits set mask=1.
      return {static_cast<uint8_t>(0x01U), sppu::regs::kStat77VersionMask};
    }
    case sppu::regs::kStat78: {
      // Bits 3:0 = PPU2 version (3 on real HW; use 3), bit 4 = NTSC/PAL
      // (0=NTSC), bits 5:6 open-bus, bit 7 = interlace field toggle.
      uint8_t value = 0x03U;  // version
      if (field_) {
        value = static_cast<uint8_t>(value | sppu::regs::kStat78FieldMask);
      }
      const uint8_t driven = static_cast<uint8_t>(sppu::regs::kStat78VersionMask | sppu::regs::kStat78PalMask |
                                                  sppu::regs::kStat78FieldMask);
      return {value, driven};
    }
    default:
      // Every other port in $2100-$213F reads as pure open-bus per
      // "correct from the start": write-only registers (INIDISP, VMAIN, etc.)
      // and stubs-in-logic ($2134-$2137 multiplier / SLHV, $213C-$213D
      // H/V latch) all surface bus-driven bits rather than returning zero.
      return {0x00U, 0x00U};
  }
}

void Ppu::WriteRegister(uint32_t offset, uint8_t data, TimeMasterT current_time) {
  const uint16_t reg = static_cast<uint16_t>(offset & 0xFFFFU);
  if (reg < sppu::regs::kBase || reg >= sppu::regs::kEnd) {
    if (reg >= ApuStub::kPortBase && reg < ApuStub::kPortEnd && snes_ != nullptr && snes_->apu_stub != nullptr) {
      snes_->apu_stub->WriteRegister(reg, data, current_time);
    }
    // Writes to $2180-$21FF drop until WRAM-port device lands.
    return;
  }
  // Mirror the written byte into the shadow immediately so the debugger can
  // observe last-written-value without waiting for log replay. Decoded state
  // (`forced_blank_`, latches, etc.) only updates when the pending-write log
  // replays — that happens inside Tick during catch-up.
  shadow_[static_cast<std::size_t>(reg - sppu::regs::kBase)] = data;
  (void)EnqueueWrite(reg, data, current_time);
}

FrameBufferView Ppu::BuildFrontView() const {
  const uint32_t logical_height = (force_overscan_draw_ || overscan_) ? 239U : 224U;
  // Point at the first visible pixel (H=22, V=1) of the grid so consumers can
  // treat the view as a logical 256×(224|239) image. The full 341×313 grid
  // stays accessible via GetFrontBuffer() for overlays that need H/V-blank
  // regions.
  const std::size_t visible_origin =
      static_cast<std::size_t>(sppu::regs::kVisibleVStartNtsc) * sppu::regs::kFrameBufferWidth +
      static_cast<std::size_t>(sppu::regs::kVisibleHStart);
  return {front_buffer_->data() + visible_origin, sppu::regs::kLogicalWidth, logical_height,
          sppu::regs::kFrameBufferWidth};
}

bool Ppu::EnqueueWrite(uint16_t offset, uint8_t data, TimeMasterT cycle) {
  if (pending_writes_count_ >= kPendingWriteLogSize) {
    // Soft-limit flush: the log filled without any intervening read/Tick
    // draining it. Apply everything synchronously up to the new entry's
    // cycle so no write is lost. Because enqueues are monotonic in cycle,
    // all queued entries have cycle <= this one and the drain clears the
    // whole log. The pixel-accurate replay timing for any pixels the dot
    // loop hasn't yet emitted is traded away here — this is a fail-safe,
    // not the hot path.
    DrainPendingWritesUpTo(cycle);
  }
  (*pending_writes_)[pending_writes_count_++] = {cycle, offset, data};
  return true;
}

void Ppu::DrainPendingWritesUpTo(TimeMasterT cutoff) {
  // Entries are appended monotonically by bus time, so we can scan forward
  // from the cursor and stop at the first entry past the cutoff. Once the
  // cursor reaches the tail, reset both indices so the next enqueue starts
  // at the beginning of the array.
  while (pending_writes_cursor_ < pending_writes_count_) {
    const PpuPokeLogEntry& entry = (*pending_writes_)[pending_writes_cursor_];
    if (entry.cycle > cutoff) {
      break;
    }
    ReplayWrite(entry.offset, entry.data);
    ++pending_writes_cursor_;
  }
  if (pending_writes_cursor_ == pending_writes_count_) {
    pending_writes_cursor_ = 0;
    pending_writes_count_ = 0;
  }
}

void Ppu::AdvanceHv() {
  ++h_;
  if (h_ >= sppu::regs::kDotsPerLine) {
    h_ = 0;
    ++v_;
    if (v_ >= sppu::regs::kLinesPerFrameNtsc) {
      v_ = 0;
    }
    // Drive the VBlank NMI latch off live scanline transitions. Arm it the
    // instant V steps onto the first VBlank line; clear it at the frame-start
    // wrap so a subsequent VBlank can re-arm. Real hardware fires a pulse
    // from the NMI line at this boundary; we only need the latch until CPU
    // interrupt delivery lands.
    const uint32_t vblank_start = overscan_ ? sppu::regs::kVisibleVEnd239 : sppu::regs::kVisibleVEnd224;
    if (v_ == vblank_start) {
      vblank_nmi_flag_ = true;
    } else if (v_ == 0) {
      vblank_nmi_flag_ = false;
    }
  }
}

void Ppu::EmitPixel(uint32_t h, uint32_t v) {
  const bool in_visible_h = (h >= sppu::regs::kVisibleHStart && h < sppu::regs::kVisibleHEnd);
  const uint32_t v_end =
      (force_overscan_draw_ || overscan_) ? sppu::regs::kVisibleVEnd239 : sppu::regs::kVisibleVEnd224;
  const bool in_visible_v = (v >= sppu::regs::kVisibleVStartNtsc && v < v_end);

  uint16_t color = 0;
  if (in_visible_h && in_visible_v && !forced_blank_) {
    uint16_t bgr = (*cgram_)[0];

    // Slot is either a BG fetch at a specific priority, or the resolved OBJ
    // pixel at a specific priority (0..3). is_obj==true uses `priority` as
    // the OBJ priority level; is_obj==false uses `bg` (0..3) and `priority`
    // as the BG priority bit (0 or 1).
    struct Slot {
      bool is_obj;
      uint8_t bg;
      uint8_t priority;
    };

    const uint32_t screen_x = h - sppu::regs::kVisibleHStart;
    const uint32_t screen_y = v - sppu::regs::kVisibleVStartNtsc;

    ObjPixel obj_px = {0U, true, 0U};
    if ((main_screen_layers_ & sppu::regs::kTmObjMask) != 0U) {
      obj_px = FetchObjPixel(screen_x, screen_y);
    }

    auto try_resolve = [&](auto order, auto bpp_for, auto cgram_base_for) {
      for (const Slot slot : order) {
        if (slot.is_obj) {
          if (obj_px.transparent) continue;
          if (obj_px.priority != slot.priority) continue;
          bgr = (*cgram_)[obj_px.cgram_index];
          return true;
        }
        if ((main_screen_layers_ & kTmMaskForBg[slot.bg]) == 0U) {
          continue;
        }
        const BgPixel px = FetchBgPixel(slot.bg, bpp_for(slot.bg), screen_x, screen_y);
        if (px.transparent || static_cast<uint8_t>(px.priority) != slot.priority) {
          continue;
        }
        const uint8_t cgram_index = static_cast<uint8_t>(px.cgram_index + cgram_base_for(slot.bg));
        bgr = (*cgram_)[cgram_index];
        return true;
      }
      return false;
    };

    if (bg_mode_ == 0U) {
      // Mode 0 priority order with sprites interleaved (highest first):
      //   OBJ.3, BG1.h, BG2.h, OBJ.2, BG1.l, BG2.l,
      //   OBJ.1, BG3.h, BG4.h, OBJ.0, BG3.l, BG4.l, backdrop.
      // BG palette regions: BG1=0, BG2=+32, BG3=+64, BG4=+96.
      static constexpr std::array<Slot, 12> kOrder = {{
          {true, 0U, 3U},
          {false, 0U, 1U},
          {false, 1U, 1U},
          {true, 0U, 2U},
          {false, 0U, 0U},
          {false, 1U, 0U},
          {true, 0U, 1U},
          {false, 2U, 1U},
          {false, 3U, 1U},
          {true, 0U, 0U},
          {false, 2U, 0U},
          {false, 3U, 0U},
      }};
      try_resolve(
          kOrder, [](uint8_t /*bg*/) -> uint8_t { return 2U; },
          [](uint8_t bg) -> uint8_t { return static_cast<uint8_t>(bg * 32U); });
    } else if (bg_mode_ == 1U) {
      // Mode 1 priority orders with sprites:
      //   bg3_priority_ off:
      //     OBJ.3, BG1.h, BG2.h, OBJ.2, BG1.l, BG2.l,
      //     OBJ.1, BG3.h, OBJ.0, BG3.l, backdrop.
      //   bg3_priority_ on (BGMODE bit 3):
      //     BG3.h, OBJ.3, BG1.h, BG2.h, OBJ.2, BG1.l, BG2.l,
      //     OBJ.1, OBJ.0, BG3.l, backdrop.
      static constexpr std::array<Slot, 10> kOrderNormal = {{
          {true, 0U, 3U},
          {false, 0U, 1U},
          {false, 1U, 1U},
          {true, 0U, 2U},
          {false, 0U, 0U},
          {false, 1U, 0U},
          {true, 0U, 1U},
          {false, 2U, 1U},
          {true, 0U, 0U},
          {false, 2U, 0U},
      }};
      static constexpr std::array<Slot, 10> kOrderBg3High = {{
          {false, 2U, 1U},
          {true, 0U, 3U},
          {false, 0U, 1U},
          {false, 1U, 1U},
          {true, 0U, 2U},
          {false, 0U, 0U},
          {false, 1U, 0U},
          {true, 0U, 1U},
          {true, 0U, 0U},
          {false, 2U, 0U},
      }};
      auto bpp_for = [](uint8_t bg) -> uint8_t { return (bg == 2U) ? 2U : 4U; };
      auto cgram_base_for = [](uint8_t /*bg*/) -> uint8_t { return 0U; };
      if (bg3_priority_) {
        try_resolve(kOrderBg3High, bpp_for, cgram_base_for);
      } else {
        try_resolve(kOrderNormal, bpp_for, cgram_base_for);
      }
    }

    color = BrightnessScale(bgr, brightness_);
  }
  // Outside the visible window and under forced-blank, the PPU drives black.
  (*back_buffer_)[static_cast<std::size_t>(v) * sppu::regs::kFrameBufferWidth + h] = color;
}

void Ppu::OnEndOfFrame() {
  std::swap(front_buffer_, back_buffer_);
  if (snes_ != nullptr) {
    snes_->FireFrameReady(BuildFrontView());
  }
}

void Ppu::ReplayWrite(uint16_t offset, uint8_t data) {
  switch (offset) {
    case sppu::regs::kInidisp:
      forced_blank_ = (data & sppu::regs::kInidispForcedBlankMask) != 0U;
      brightness_ = data & sppu::regs::kInidispBrightnessMask;
      break;

    case sppu::regs::kOamAddL: {
      const uint32_t high_bit = static_cast<uint32_t>(oam_byte_addr_reload_ & 0x200U);
      const uint32_t low_bits = static_cast<uint32_t>(data) << 1U;
      const uint16_t new_word_addr = static_cast<uint16_t>(high_bit | low_bits);
      oam_byte_addr_reload_ = new_word_addr;
      oam_byte_addr_ = new_word_addr;
      break;
    }
    case sppu::regs::kOamAddH: {
      oam_priority_rotation_ = (data & sppu::regs::kOamAddHPriorityRotateMask) != 0U;
      const uint32_t high_bit = (static_cast<uint32_t>(data) & 0x01U) << 9U;
      const uint32_t low_bits = static_cast<uint32_t>(oam_byte_addr_reload_ & 0x1FFU);
      const uint16_t new_word_addr = static_cast<uint16_t>(high_bit | low_bits);
      oam_byte_addr_reload_ = new_word_addr;
      oam_byte_addr_ = new_word_addr;
      break;
    }
    case sppu::regs::kOamData: {
      const uint16_t addr = oam_byte_addr_;
      if (addr < 0x200U) {
        // Low OAM: write-twice. Low byte latches; high byte commits both
        // bytes of the current word.
        if ((addr & 1U) == 0U) {
          oam_write_latch_ = data;
        } else {
          WriteOamByte(static_cast<uint16_t>(addr - 1U), oam_write_latch_);
          WriteOamByte(addr, data);
        }
      } else {
        // High table: direct byte write, no latch.
        WriteOamByte(addr, data);
      }
      oam_byte_addr_ = static_cast<uint16_t>((addr + 1U) & 0x3FFU);
      break;
    }

    case sppu::regs::kVmain: vmain_ = data; break;
    case sppu::regs::kVmAddL:
      vmadd_ = static_cast<uint16_t>((vmadd_ & 0xFF00U) | data);
      PrefetchVram();
      break;
    case sppu::regs::kVmAddH: {
      const uint32_t low_byte = static_cast<uint32_t>(vmadd_ & 0x00FFU);
      const uint32_t high_byte = static_cast<uint32_t>(data) << 8U;
      vmadd_ = static_cast<uint16_t>(low_byte | high_byte);
      PrefetchVram();
      break;
    }
    case sppu::regs::kVmDataL: {
      const uint16_t byte_addr = static_cast<uint16_t>(static_cast<uint32_t>(TranslateVramAddress(vmadd_)) << 1U);
      (*vram_)[byte_addr] = data;
      MaybeIncrementVmaddOnPort(/*is_high_port=*/false);
      break;
    }
    case sppu::regs::kVmDataH: {
      const uint32_t word_addr_shifted = static_cast<uint32_t>(TranslateVramAddress(vmadd_)) << 1U;
      const uint16_t byte_addr = static_cast<uint16_t>(word_addr_shifted | 1U);
      (*vram_)[byte_addr] = data;
      MaybeIncrementVmaddOnPort(/*is_high_port=*/true);
      break;
    }

    case sppu::regs::kCgAdd:
      cgadd_ = data;
      cgram_write_latch_high_ = false;
      cgram_read_latch_high_ = false;
      break;
    case sppu::regs::kCgData:
      if (!cgram_write_latch_high_) {
        cgram_write_latch_data_ = data;
        cgram_write_latch_high_ = true;
      } else {
        // Commit the full 15-bit BGR word. Bit 15 is forced to 0 — CGRAM
        // words only have 15 meaningful bits (BGR555). fullsnes confirms the
        // stored MSB reads back as 0.
        const uint16_t word =
            static_cast<uint16_t>((static_cast<uint16_t>(data & 0x7FU) << 8) | cgram_write_latch_data_);
        (*cgram_)[cgadd_] = word;
        cgadd_ = static_cast<uint8_t>(cgadd_ + 1U);
        cgram_write_latch_high_ = false;
      }
      break;

    case sppu::regs::kSetini: overscan_ = (data & sppu::regs::kSetiniOverscanMask) != 0U; break;

    case sppu::regs::kObsel: {
      obj_size_select_ = static_cast<uint8_t>((data & sppu::regs::kObselSizeMask) >> sppu::regs::kObselSizeShift);
      const uint32_t name_select =
          static_cast<uint32_t>((data & sppu::regs::kObselNameSelectMask) >> sppu::regs::kObselNameSelectShift);
      const uint32_t name_base = static_cast<uint32_t>(data & sppu::regs::kObselNameBaseMask);
      // Region 0 base = name_base * 0x2000 words. Region 1 = region0 + 0x1000
      // + name_select * 0x1000. Both wrap modulo 32K word VRAM.
      obj_region0_word_ = static_cast<uint16_t>((name_base << 13U) & 0x7FFFU);
      obj_region1_word_ =
          static_cast<uint16_t>((static_cast<uint32_t>(obj_region0_word_) + 0x1000U + (name_select << 12U)) & 0x7FFFU);
      break;
    }

    case sppu::regs::kBgmode:
      bg_mode_ = data & sppu::regs::kBgmodeModeMask;
      bg3_priority_ = (data & sppu::regs::kBgmodeBg3PriorityMask) != 0U;
      bg_tile_16x16_[0] = (data & sppu::regs::kBgmodeBg1TileSizeMask) != 0U;
      bg_tile_16x16_[1] = (data & sppu::regs::kBgmodeBg2TileSizeMask) != 0U;
      bg_tile_16x16_[2] = (data & sppu::regs::kBgmodeBg3TileSizeMask) != 0U;
      bg_tile_16x16_[3] = (data & sppu::regs::kBgmodeBg4TileSizeMask) != 0U;
      break;

    case sppu::regs::kBg1Sc:
    case sppu::regs::kBg2Sc:
    case sppu::regs::kBg3Sc:
    case sppu::regs::kBg4Sc: {
      const std::size_t bg = static_cast<std::size_t>(offset - sppu::regs::kBg1Sc);
      // Bits 7:2 are the screen base in 1K-word steps → word base = bits<<10.
      bg_tilemap_word_base_[bg] = static_cast<uint16_t>(static_cast<uint16_t>(data & sppu::regs::kBgScBaseMask) << 8);
      bg_tilemap_layout_[bg] = static_cast<uint8_t>(data & sppu::regs::kBgScLayoutMask);
      break;
    }

    case sppu::regs::kBg12Nba:
      // Low nibble = BG1, high nibble = BG2. Each nibble × 0x1000 word steps.
      bg_char_word_base_[0] = static_cast<uint16_t>(static_cast<uint16_t>(data & 0x0FU) << 12);
      bg_char_word_base_[1] = static_cast<uint16_t>(static_cast<uint16_t>((data >> 4) & 0x0FU) << 12);
      break;
    case sppu::regs::kBg34Nba:
      bg_char_word_base_[2] = static_cast<uint16_t>(static_cast<uint16_t>(data & 0x0FU) << 12);
      bg_char_word_base_[3] = static_cast<uint16_t>(static_cast<uint16_t>((data >> 4) & 0x0FU) << 12);
      break;

    case sppu::regs::kBg1Hofs:
    case sppu::regs::kBg2Hofs:
    case sppu::regs::kBg3Hofs:
    case sppu::regs::kBg4Hofs: {
      // BG_old shared latch + this BG's old high byte for low 3 bits. Per
      // fullsnes: BGnHOFS = (Curr<<8) | (Prev & ~7) | ((Reg_old>>8) & 7).
      const std::size_t bg = static_cast<std::size_t>((offset - sppu::regs::kBg1Hofs) >> 1U);
      const uint16_t old_value = bg_hofs_[bg];
      const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(data) << 8);
      const uint16_t mid = static_cast<uint16_t>(bg_scroll_prev_ & 0xF8U);  // Prev & ~7
      const uint16_t low = static_cast<uint16_t>((old_value >> 8) & 0x07U);
      bg_hofs_[bg] = static_cast<uint16_t>((high | mid | low) & sppu::regs::kBgScrollMask);
      bg_scroll_prev_ = data;
      break;
    }
    case sppu::regs::kBg1Vofs:
    case sppu::regs::kBg2Vofs:
    case sppu::regs::kBg3Vofs:
    case sppu::regs::kBg4Vofs: {
      // V scroll: (Curr<<8) | Prev. No old-register feedback term.
      const std::size_t bg = static_cast<std::size_t>((offset - sppu::regs::kBg1Vofs) >> 1U);
      const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(data) << 8);
      const uint16_t low = static_cast<uint16_t>(bg_scroll_prev_);
      bg_vofs_[bg] = static_cast<uint16_t>((high | low) & sppu::regs::kBgScrollMask);
      bg_scroll_prev_ = data;
      break;
    }

    case sppu::regs::kTm: main_screen_layers_ = data; break;

    default:
      // Every other $2100-$213F write is shadow-only in v1. The shadow was
      // already updated at enqueue time, so nothing to do here.
      break;
  }
}

uint16_t Ppu::TranslateVramAddress(uint16_t raw) const {
  // VMAIN bits 3:2 choose one of four address rotations (fullsnes "F" field):
  //   00: no translation
  //   01: 8×8  2bpp — rotate low 8 bits: aaaaaaaa YYYxxxxx -> aaaaaaaa xxxxxYYY
  //   10: 8×8  4bpp — rotate low 9 bits: aaaaaaa YYYxxxxxx -> aaaaaaa xxxxxxYYY
  //   11: 8×8  8bpp — rotate low 10 bits: aaaaaa YYYxxxxxxx -> aaaaaa xxxxxxxYYY
  const uint8_t mode =
      static_cast<uint8_t>((vmain_ & sppu::regs::kVmainTranslateMask) >> sppu::regs::kVmainTranslateShift);
  switch (mode) {
    case 0: return raw;
    case 1: {
      const uint16_t high = static_cast<uint16_t>(raw & 0xFF00U);
      const uint16_t rotated = static_cast<uint16_t>(((raw & 0x00E0U) >> 5) | ((raw & 0x001FU) << 3));
      return static_cast<uint16_t>(high | rotated);
    }
    case 2: {
      const uint16_t high = static_cast<uint16_t>(raw & 0xFE00U);
      const uint16_t rotated = static_cast<uint16_t>(((raw & 0x01C0U) >> 6) | ((raw & 0x003FU) << 3));
      return static_cast<uint16_t>(high | rotated);
    }
    case 3: {
      const uint16_t high = static_cast<uint16_t>(raw & 0xFC00U);
      const uint16_t rotated = static_cast<uint16_t>(((raw & 0x0380U) >> 7) | ((raw & 0x007FU) << 3));
      return static_cast<uint16_t>(high | rotated);
    }
    default: return raw;
  }
}

uint16_t Ppu::VmainIncrementStep() const {
  switch (vmain_ & sppu::regs::kVmainStepMask) {
    case 0x00U: return 1;
    case 0x01U: return 32;
    case 0x02U:
    case 0x03U: return 128;
    default: return 1;
  }
}

void Ppu::PrefetchVram() { vram_prefetch_ = ReadVramWord(TranslateVramAddress(vmadd_)); }

void Ppu::MaybeIncrementVmaddOnPort(bool is_high_port) {
  const bool increment_on_high = (vmain_ & sppu::regs::kVmainIncrementOnHighMask) != 0U;
  if (is_high_port == increment_on_high) {
    vmadd_ = static_cast<uint16_t>(vmadd_ + VmainIncrementStep());
  }
}

void Ppu::WriteOamByte(uint16_t byte_addr, uint8_t data) { (*oam_)[OamByteSlot(byte_addr)] = data; }

uint8_t Ppu::ReadOamByte(uint16_t byte_addr) const { return (*oam_)[OamByteSlot(byte_addr)]; }

uint16_t Ppu::ReadVramWord(uint16_t word_addr) const {
  // VRAM is 32K words = 64K bytes; word index wraps at 15 bits. Low byte at
  // 2*word_addr, high byte at +1.
  const uint16_t masked = static_cast<uint16_t>(word_addr & 0x7FFFU);
  const std::size_t lo_idx = static_cast<std::size_t>(masked) << 1U;
  const uint8_t lo = (*vram_)[lo_idx];
  const uint8_t hi = (*vram_)[lo_idx | 1U];
  return PackWord(lo, hi);
}

Ppu::BgPixel Ppu::FetchBgPixel(uint8_t bg, uint8_t bpp, uint32_t screen_x, uint32_t screen_y) const {
  if (bg >= 4U) {
    return {0U, true, false};
  }
  const bool bg_is_2bpp = (bpp == 2U);

  const uint32_t tile_w = bg_tile_16x16_[bg] ? 16U : 8U;
  const uint32_t tile_h = tile_w;  // SNES BG tiles are square.

  const uint32_t eff_x = (screen_x + bg_hofs_[bg]) & 0x3FFU;  // 10-bit wrap (covers 64-tile width).
  const uint32_t eff_y = (screen_y + bg_vofs_[bg]) & 0x3FFU;

  const uint32_t tile_x = eff_x / tile_w;
  const uint32_t tile_y = eff_y / tile_h;

  // BGxSC layout:
  //   0 (32x32): single screen
  //   1 (64x32): SC0|SC1 horizontally, second screen at +0x400 words
  //   2 (32x64): SC0/SC1 vertically,  second screen at +0x400
  //   3 (64x64): 2x2, TR=+0x400, BL=+0x800, BR=+0xC00
  const uint8_t layout = bg_tilemap_layout_[bg];
  const bool wide = (layout == 1U) || (layout == 3U);
  const bool tall = (layout == 2U) || (layout == 3U);
  const uint32_t tile_x_wrapped = tile_x & (wide ? 0x3FU : 0x1FU);
  const uint32_t tile_y_wrapped = tile_y & (tall ? 0x3FU : 0x1FU);
  const uint32_t screen_col = (tile_x_wrapped >> 5U) & 0x1U;
  const uint32_t screen_row = (tile_y_wrapped >> 5U) & 0x1U;
  const uint32_t local_x = tile_x_wrapped & 0x1FU;
  const uint32_t local_y = tile_y_wrapped & 0x1FU;
  uint32_t screen_offset_words = 0;
  if (layout == 1U) {
    screen_offset_words = screen_col * 0x400U;
  } else if (layout == 2U) {
    screen_offset_words = screen_row * 0x400U;
  } else if (layout == 3U) {
    screen_offset_words = (screen_row * 0x800U) + (screen_col * 0x400U);
  }

  const uint32_t tilemap_word_addr =
      static_cast<uint32_t>(bg_tilemap_word_base_[bg]) + screen_offset_words + (local_y * 32U) + local_x;
  const uint16_t entry = ReadVramWord(static_cast<uint16_t>(tilemap_word_addr));

  uint16_t char_index = static_cast<uint16_t>(entry & sppu::regs::kBgMapEntryCharMask);
  const uint8_t palette_group =
      static_cast<uint8_t>((entry >> sppu::regs::kBgMapEntryPaletteShift) & sppu::regs::kBgMapEntryPaletteMask);
  const bool priority = (entry & sppu::regs::kBgMapEntryPriorityMask) != 0U;
  const bool hflip = (entry & sppu::regs::kBgMapEntryHflipMask) != 0U;
  const bool vflip = (entry & sppu::regs::kBgMapEntryVflipMask) != 0U;

  uint32_t pixel_in_x = eff_x % tile_w;
  uint32_t pixel_in_y = eff_y % tile_h;
  if (hflip) pixel_in_x = (tile_w - 1U) - pixel_in_x;
  if (vflip) pixel_in_y = (tile_h - 1U) - pixel_in_y;

  // 16x16: pick one of four 8x8 sub-tiles (TR=+1, BL=+0x10, BR=+0x11). The
  // hflip/vflip above already mapped pixel_in_x/y to post-flip coords, so the
  // sub-tile picks naturally.
  if (tile_w == 16U) {
    const uint16_t sub_x = static_cast<uint16_t>(pixel_in_x >> 3U);
    const uint16_t sub_y = static_cast<uint16_t>(pixel_in_y >> 3U);
    char_index = static_cast<uint16_t>(char_index + sub_x + (sub_y << 4U));
    pixel_in_x &= 7U;
    pixel_in_y &= 7U;
  }

  const uint32_t bytes_per_char = bg_is_2bpp ? 16U : 32U;
  const uint32_t char_byte_base = static_cast<uint32_t>(bg_char_word_base_[bg]) << 1U;
  const uint32_t tile_byte_addr =
      char_byte_base + (static_cast<uint32_t>(char_index) * bytes_per_char) + (pixel_in_y * 2U);

  auto vram_byte = [&](uint32_t addr) { return (*vram_)[addr & 0xFFFFU]; };

  // Planar layout: planes 0/1 interleaved at +0..+15, planes 2/3 at +16..+31.
  const uint8_t shift = static_cast<uint8_t>(7U - pixel_in_x);
  const uint8_t p0 = (vram_byte(tile_byte_addr + 0U) >> shift) & 1U;
  const uint8_t p1 = (vram_byte(tile_byte_addr + 1U) >> shift) & 1U;
  uint8_t color_index = static_cast<uint8_t>(p0 | (p1 << 1U));
  if (!bg_is_2bpp) {
    const uint8_t p2 = (vram_byte(tile_byte_addr + 16U) >> shift) & 1U;
    const uint8_t p3 = (vram_byte(tile_byte_addr + 17U) >> shift) & 1U;
    color_index = static_cast<uint8_t>(color_index | (p2 << 2U) | (p3 << 3U));
  }

  if (color_index == 0U) {
    return {0U, true, priority};
  }
  const uint8_t cgram_index = static_cast<uint8_t>((static_cast<uint8_t>(palette_group) << bpp) | color_index);
  return {cgram_index, false, priority};
}

namespace {

// OBSEL size pairs (small, large) per fullsnes table at $2101. Codes 6/7 are
// "undocumented" but present on real hardware.
struct ObjSizePair {
  uint8_t small_w;
  uint8_t small_h;
  uint8_t large_w;
  uint8_t large_h;
};
constexpr std::array<ObjSizePair, 8> kObjSizes = {{
    {8, 8, 16, 16},    // 0
    {8, 8, 32, 32},    // 1
    {8, 8, 64, 64},    // 2
    {16, 16, 32, 32},  // 3
    {16, 16, 64, 64},  // 4
    {32, 32, 64, 64},  // 5
    {16, 32, 32, 64},  // 6 (undocumented)
    {16, 32, 32, 32},  // 7 (undocumented)
}};

}  // namespace

Ppu::ObjPixel Ppu::FetchObjPixel(uint32_t screen_x, uint32_t screen_y) const {
  const ObjSizePair sizes = kObjSizes[obj_size_select_];

  for (uint8_t obj = 0; obj < 128U; ++obj) {
    const uint16_t low_addr = static_cast<uint16_t>(obj * 4U);

    // High-table byte holds (X-high, size) bit-pair at position (obj%4)*2 for
    // four OBJs at a time. Read it + Y first so the common "no Y overlap"
    // miss path skips the X/tile/attr reads.
    const uint16_t high_byte_addr = static_cast<uint16_t>(sppu::regs::kOamHighTableBase + (obj >> 2U));
    const uint8_t high_byte = ReadOamByte(high_byte_addr);
    const uint8_t high_shift = static_cast<uint8_t>((obj & 3U) << 1U);
    const bool large = ((high_byte >> (high_shift + 1U)) & 1U) != 0U;

    const uint8_t width = large ? sizes.large_w : sizes.small_w;
    const uint8_t height = large ? sizes.large_h : sizes.small_h;

    const uint8_t y_raw = ReadOamByte(static_cast<uint16_t>(low_addr + 1U));
    // Y range with 8-bit wrap (sprites can wrap bottom→top).
    const uint8_t y_internal_8 = static_cast<uint8_t>(static_cast<uint8_t>(screen_y) - y_raw);
    if (y_internal_8 >= height) continue;

    const bool x_high_bit = ((high_byte >> high_shift) & 1U) != 0U;
    const uint8_t x_lo = ReadOamByte(low_addr);
    // X is 9-bit signed (sign-extend bit 8). Sprites can sit partly off-screen.
    int32_t signed_x = static_cast<int32_t>(x_lo) | (x_high_bit ? -256 : 0);
    const int32_t x_internal = static_cast<int32_t>(screen_x) - signed_x;
    if (x_internal < 0 || x_internal >= width) continue;

    const uint8_t tile_lo = ReadOamByte(static_cast<uint16_t>(low_addr + 2U));
    const uint8_t attr = ReadOamByte(static_cast<uint16_t>(low_addr + 3U));

    const bool hflip = (attr & sppu::regs::kObjAttrHflipMask) != 0U;
    const bool vflip = (attr & sppu::regs::kObjAttrVflipMask) != 0U;
    uint32_t pix_x = static_cast<uint32_t>(x_internal);
    uint32_t pix_y = static_cast<uint32_t>(y_internal_8);
    if (hflip) pix_x = (static_cast<uint32_t>(width) - 1U) - pix_x;
    if (vflip) pix_y = (static_cast<uint32_t>(height) - 1U) - pix_y;

    const uint32_t sub_x = pix_x >> 3U;
    const uint32_t sub_y = pix_y >> 3U;
    const uint32_t in_x = pix_x & 7U;
    const uint32_t in_y = pix_y & 7U;

    // 9-bit tile number (bit 8 from attr.0). Low nibble wraps within its
    // 16-tile row; high nibble wraps within its 16-row page; the region-select
    // bit does NOT carry.
    const uint16_t base_tile = static_cast<uint16_t>(tile_lo | ((attr & sppu::regs::kObjAttrTileHighMask) << 8U));
    const uint16_t tile_x_low = static_cast<uint16_t>(((base_tile & 0x0FU) + sub_x) & 0x0FU);
    const uint16_t tile_y_low = static_cast<uint16_t>((((base_tile >> 4U) & 0x0FU) + sub_y) & 0x0FU);
    const uint16_t region_bit = static_cast<uint16_t>(base_tile & 0x100U);
    const uint16_t effective_tile = static_cast<uint16_t>(region_bit | (tile_y_low << 4U) | tile_x_low);

    const uint16_t region_word_base = (effective_tile & 0x100U) ? obj_region1_word_ : obj_region0_word_;
    const uint16_t tile_in_region = static_cast<uint16_t>(effective_tile & 0xFFU);
    const uint32_t tile_byte_addr =
        (static_cast<uint32_t>(region_word_base) << 1U) + (static_cast<uint32_t>(tile_in_region) * 32U) + (in_y * 2U);

    auto vram_byte = [&](uint32_t addr) { return (*vram_)[addr & 0xFFFFU]; };

    const uint8_t shift = static_cast<uint8_t>(7U - in_x);
    const uint8_t p0 = (vram_byte(tile_byte_addr + 0U) >> shift) & 1U;
    const uint8_t p1 = (vram_byte(tile_byte_addr + 1U) >> shift) & 1U;
    const uint8_t p2 = (vram_byte(tile_byte_addr + 16U) >> shift) & 1U;
    const uint8_t p3 = (vram_byte(tile_byte_addr + 17U) >> shift) & 1U;
    const uint8_t color_index = static_cast<uint8_t>(p0 | (p1 << 1U) | (p2 << 2U) | (p3 << 3U));
    if (color_index == 0U) {
      continue;
    }

    // OBJ palette region: $80..$FF, eight 16-color groups.
    const unsigned palette_group =
        (static_cast<unsigned>(attr) >> sppu::regs::kObjAttrPaletteShift) & sppu::regs::kObjAttrPaletteMask;
    const uint8_t cgram_index =
        static_cast<uint8_t>(0x80U | (palette_group << 4U) | static_cast<unsigned>(color_index));
    const uint8_t priority = static_cast<uint8_t>(
        (static_cast<unsigned>(attr) >> sppu::regs::kObjAttrPriorityShift) & sppu::regs::kObjAttrPriorityMask);
    return {cgram_index, false, priority};
  }

  return {0U, true, 0U};
}

}  // namespace pupsnes
