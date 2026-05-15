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

constexpr std::array<uint8_t, sppu::regs::kBgCount> kTmMaskForBg = {
    sppu::regs::kTmBg1Mask, sppu::regs::kTmBg2Mask, sppu::regs::kTmBg3Mask, sppu::regs::kTmBg4Mask};

// 5-bit BGR555 channels.
struct Bgr5 {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};
constexpr Bgr5 UnpackBgr555(uint16_t bgr) {
  return {static_cast<uint8_t>(bgr & 0x1FU), static_cast<uint8_t>((bgr >> 5U) & 0x1FU),
          static_cast<uint8_t>((bgr >> 10U) & 0x1FU)};
}
constexpr uint16_t PackBgr555(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(r | (static_cast<uint16_t>(g) << 5U) | (static_cast<uint16_t>(b) << 10U));
}

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
  sub_screen_layers_ = 0;
  cgwsel_ = 0;
  cgadsub_ = 0;
  coldata_r_ = 0;
  coldata_g_ = 0;
  coldata_b_ = 0;

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

  // Per-scanline OAM cache: -1 sentinel forces a rebuild on the next OBJ fetch.
  obj_line_v_ = -1;
  obj_line_count_ = 0;

  // Per-BG row cache: -1 sentinel + dirty=true forces a refetch.
  for (auto& c : bg_row_cache_) c = {};
  bg_row_dirty_.fill(true);

  vblank_nmi_flag_ = false;

  ophct_ = 0;
  opvct_ = 0;
  ophct_read_high_ = false;
  opvct_read_high_ = false;
  hv_latch_flag_ = false;

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
    snes_->scheduler->ScheduleSignal(NextFramePeriod(), SignalKind::kFrameEnd,
                                     [this](TimeMasterT t) { OnFrameEndSignal(t); });
    // VBlank-NMI boundary: the CPU's NMI flip-flop is edge-triggered on the
    // /NMI line's falling edge, which lands when V transitions onto the
    // VBlank entry line (225 normally, 240 with SETINI overscan — SETINI
    // starts clear at reset so V=225). Scheduling this as a scheduler signal
    // gives a sync fence: the CPU cannot run past the assertion cycle in a
    // single tick budget, which is the only way to guarantee it can't
    // "time-travel over" an NMI that real hardware would have delivered.
    const TimeMasterT nmi_boundary_mcyc =
        static_cast<TimeMasterT>(VblankStartLine()) * sppu::regs::kNormalLineCycles;
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
  return v_ == VblankStartLine();
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
  if (snes_ != nullptr && snes_->scheduler != nullptr) {
    snes_->scheduler->ScheduleSignal(master_time + NextFramePeriod(), SignalKind::kVblankNmiBoundary,
                                     [this](TimeMasterT t) { OnVblankNmiBoundarySignal(t); });
  }
}

PpuHvbStatus Ppu::QueryHvbStatus(TimeMasterT current_time) {
  CatchUpTo(current_time);
  // h_/v_ point at the next dot to emit after catch-up. VBlank start tracks
  // SETINI overscan live — fullsnes notes the bit can flip mid-frame; v1
  // scaffold follows the current overscan_ value rather than a per-frame
  // latch.
  const bool vblank = v_ >= VblankStartLine();
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
  snes_->scheduler->ScheduleSignal(master_time + NextFramePeriod(), SignalKind::kFrameEnd,
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
    case sppu::regs::kSlhv: {
      // Dummy-read latches the current H/V counters into OPHCT/OPVCT and
      // sets the latch flag (STAT78.bit6). Per fullsnes the gating condition
      // is WRIO.bit7 being (or having been) set; WRIO is not modeled today
      // and its reset value FFh already satisfies the gate, so we always
      // latch. The read value itself is open-bus.
      ophct_ = static_cast<uint16_t>(h_ & 0x01FFU);
      opvct_ = static_cast<uint16_t>(v_ & 0x01FFU);
      hv_latch_flag_ = true;
      return {0x00U, 0x00U};
    }
    case sppu::regs::kRdOam: {
      const uint8_t value = ReadOamByte(oam_byte_addr_);
      oam_byte_addr_ = static_cast<uint16_t>((oam_byte_addr_ + 1U) & 0x3FFU);
      return {value, 0xFFU};
    }
    case sppu::regs::kRdVramL: {
      const uint8_t value = static_cast<uint8_t>(vram_prefetch_ & 0xFFU);
      MaybeAdvanceVramReadOnPort(/*is_high_port=*/false);
      return {value, 0xFFU};
    }
    case sppu::regs::kRdVramH: {
      const uint8_t value = static_cast<uint8_t>((vram_prefetch_ >> 8) & 0xFFU);
      MaybeAdvanceVramReadOnPort(/*is_high_port=*/true);
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
    case sppu::regs::kOphct: return ReadOpct(ophct_, ophct_read_high_);
    case sppu::regs::kOpvct: return ReadOpct(opvct_, opvct_read_high_);
    case sppu::regs::kStat77: {
      // Bits 3:0 = PPU1 version (1), bits 6:4 open-bus, bit 7 time-over
      // (stubbed 0). Only the driven bits set mask=1.
      return {static_cast<uint8_t>(0x01U), sppu::regs::kStat77VersionMask};
    }
    case sppu::regs::kStat78: {
      // Bits 3:0 = PPU2 version (3 on real HW; use 3), bit 4 = NTSC/PAL
      // (0=NTSC), bit 5 open-bus, bit 6 = H/V latch flag, bit 7 = interlace
      // field toggle. Reading this register resets both OPHCT/OPVCT 1st/2nd
      // flipflops and clears the latch flag.
      uint8_t value = 0x03U;  // version
      if (field_) {
        value = static_cast<uint8_t>(value | sppu::regs::kStat78FieldMask);
      }
      if (hv_latch_flag_) {
        value = static_cast<uint8_t>(value | sppu::regs::kStat78LatchFlagMask);
      }
      const uint8_t driven = static_cast<uint8_t>(sppu::regs::kStat78VersionMask | sppu::regs::kStat78PalMask |
                                                  sppu::regs::kStat78FieldMask | sppu::regs::kStat78LatchFlagMask);
      ophct_read_high_ = false;
      opvct_read_high_ = false;
      hv_latch_flag_ = false;
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
    // Scanline transition invalidates both PPU render caches: the OAM list
    // is rebuilt at the next OBJ fetch, and each BG's row cache is cleared
    // so the next BG fetch re-reads the tilemap entry for the new pixel_in_y.
    obj_line_v_ = -1;
    for (auto& c : bg_row_cache_) c.key = -1;
    if (v_ >= sppu::regs::kLinesPerFrameNtsc) {
      v_ = 0;
    }
    // Drive the VBlank NMI latch off live scanline transitions. Arm it the
    // instant V steps onto the first VBlank line; clear it at the frame-start
    // wrap so a subsequent VBlank can re-arm. Real hardware fires a pulse
    // from the NMI line at this boundary; we only need the latch until CPU
    // interrupt delivery lands.
    if (v_ == VblankStartLine()) {
      vblank_nmi_flag_ = true;
    } else if (v_ == 0) {
      vblank_nmi_flag_ = false;
    }
  }
}

namespace {

// Priority ladder for one BG mode. Each Slot is either an OBJ slot at a
// specific OBJ priority (0..3), or a BG slot for (bg index, BG priority bit).
struct Slot {
  bool is_obj;
  uint8_t bg;
  uint8_t priority;
};

// Mode 0 priority (highest first): BG palette regions stagger by 32 entries.
constexpr std::array<Slot, 12> kOrderMode0 = {{
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

// Mode 1, BGMODE.3 clear — BG3 priority "normal".
constexpr std::array<Slot, 10> kOrderMode1Normal = {{
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

// Mode 1, BGMODE.3 set — BG3 priority-1 tiles climb above BG1/BG2.
constexpr std::array<Slot, 10> kOrderMode1Bg3High = {{
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

// 5-bit per-channel saturating add (0..31).
constexpr uint8_t SatAdd5(uint8_t a, uint8_t b) {
  const uint32_t sum = static_cast<uint32_t>(a) + static_cast<uint32_t>(b);
  return static_cast<uint8_t>(sum > 31U ? 31U : sum);
}
// 5-bit per-channel saturating subtract (clamped to 0).
constexpr uint8_t SatSub5(uint8_t a, uint8_t b) { return static_cast<uint8_t>(a > b ? a - b : 0U); }

}  // namespace

Ppu::ResolvedPixel Ppu::ResolveScreenPixel(uint8_t layer_mask, uint32_t screen_x, uint32_t screen_y,
                                           const ObjPixel& obj_px) const {
  ResolvedPixel result = {(*cgram_)[0], 5U, false};

  auto try_resolve = [&](auto order, auto bpp_for, auto cgram_base_for) -> bool {
    for (const Slot slot : order) {
      if (slot.is_obj) {
        if ((layer_mask & sppu::regs::kTmObjMask) == 0U) continue;
        if (obj_px.transparent) continue;
        if (obj_px.priority != slot.priority) continue;
        result.bgr = (*cgram_)[obj_px.cgram_index];
        result.layer_id = 4U;
        // OBJ palette 4..7 occupy CGRAM $C0..$FF — bit 6 of the absolute
        // CGRAM index disambiguates the math-eligible "high" palettes.
        result.obj_palette_high = (obj_px.cgram_index & 0x40U) != 0U;
        return true;
      }
      if ((layer_mask & kTmMaskForBg[slot.bg]) == 0U) continue;
      const BgPixel px = FetchBgPixel(slot.bg, bpp_for(slot.bg), screen_x, screen_y);
      if (px.transparent || static_cast<uint8_t>(px.priority) != slot.priority) continue;
      const uint8_t cgram_index = static_cast<uint8_t>(px.cgram_index + cgram_base_for(slot.bg));
      result.bgr = (*cgram_)[cgram_index];
      result.layer_id = slot.bg;
      return true;
    }
    return false;
  };

  if (bg_mode_ == 0U) {
    try_resolve(
        kOrderMode0, [](uint8_t /*bg*/) -> uint8_t { return 2U; },
        [](uint8_t bg) -> uint8_t { return static_cast<uint8_t>(bg * 32U); });
  } else if (bg_mode_ == 1U) {
    auto bpp_for = [](uint8_t bg) -> uint8_t { return (bg == 2U) ? 2U : 4U; };
    auto cgram_base_for = [](uint8_t /*bg*/) -> uint8_t { return 0U; };
    if (bg3_priority_) {
      try_resolve(kOrderMode1Bg3High, bpp_for, cgram_base_for);
    } else {
      try_resolve(kOrderMode1Normal, bpp_for, cgram_base_for);
    }
  }
  return result;
}

uint16_t Ppu::ApplyColorMath(uint16_t main_bgr, uint8_t main_layer, bool main_obj_high, uint32_t screen_x,
                             uint32_t screen_y, const ObjPixel& obj_px) const {
  // Determine if the main layer at this pixel is included in CGADSUB. The
  // OBJ case adds a "palette >= 4" gate per fullsnes.
  uint8_t layer_bit = 0;
  if (main_layer < 4U) {
    layer_bit = static_cast<uint8_t>(1U << main_layer);  // BG1..BG4
  } else if (main_layer == 4U) {
    layer_bit = main_obj_high ? sppu::regs::kCgadsubObjMask : 0U;
  } else {
    layer_bit = sppu::regs::kCgadsubBackdropMask;  // backdrop
  }
  if ((cgadsub_ & layer_bit) == 0U) {
    return main_bgr;
  }

  // Build the sub-screen contribution. With CGWSEL.1 set the TS layer ladder
  // is resolved; if nothing renders there, the sub-screen pixel falls back to
  // the COLDATA fixed colour. CGWSEL.1 clear short-circuits to fixed.
  uint16_t sub_bgr = PackBgr555(coldata_r_, coldata_g_, coldata_b_);
  if ((cgwsel_ & sppu::regs::kCgwselSubScreenEnableMask) != 0U) {
    const ResolvedPixel sub = ResolveScreenPixel(sub_screen_layers_, screen_x, screen_y, obj_px);
    if (sub.layer_id != 5U) {
      sub_bgr = sub.bgr;
    }
  }

  // Split BGR555 into per-channel intensities, apply add or subtract with
  // saturation, then optionally halve the final per channel.
  const Bgr5 m = UnpackBgr555(main_bgr);
  const Bgr5 s = UnpackBgr555(sub_bgr);

  const bool subtract = (cgadsub_ & sppu::regs::kCgadsubSubtractMask) != 0U;
  uint8_t r = subtract ? SatSub5(m.r, s.r) : SatAdd5(m.r, s.r);
  uint8_t g = subtract ? SatSub5(m.g, s.g) : SatAdd5(m.g, s.g);
  uint8_t b = subtract ? SatSub5(m.b, s.b) : SatAdd5(m.b, s.b);

  if ((cgadsub_ & sppu::regs::kCgadsubHalfMask) != 0U) {
    // Per-channel right shift. fullsnes also documents a "skip halve when the
    // sub-screen is the fixed-COLDATA fallback" quirk; v1 doesn't model that
    // and applies the halve unconditionally when the bit is set.
    r >>= 1U;
    g >>= 1U;
    b >>= 1U;
  }
  return PackBgr555(r, g, b);
}

void Ppu::EmitPixel(uint32_t h, uint32_t v) {
  const bool in_visible_h = (h >= sppu::regs::kVisibleHStart && h < sppu::regs::kVisibleHEnd);
  const uint32_t v_end =
      (force_overscan_draw_ || overscan_) ? sppu::regs::kVisibleVEnd239 : sppu::regs::kVisibleVEnd224;
  const bool in_visible_v = (v >= sppu::regs::kVisibleVStartNtsc && v < v_end);

  uint16_t color = 0;
  if (in_visible_h && in_visible_v && !forced_blank_) {
    const uint32_t screen_x = h - sppu::regs::kVisibleHStart;
    const uint32_t screen_y = v - sppu::regs::kVisibleVStartNtsc;

    // FetchObjPixel is gated on either TM or TS enabling OBJ, since the same
    // resolved sprite pixel feeds both main and sub resolutions.
    const uint8_t obj_enable_mask = static_cast<uint8_t>(main_screen_layers_ | sub_screen_layers_);
    ObjPixel obj_px = {0U, true, 0U};
    if ((obj_enable_mask & sppu::regs::kTmObjMask) != 0U) {
      obj_px = FetchObjPixel(screen_x, screen_y);
    }

    const ResolvedPixel main = ResolveScreenPixel(main_screen_layers_, screen_x, screen_y, obj_px);
    const uint16_t composed =
        ApplyColorMath(main.bgr, main.layer_id, main.obj_palette_high, screen_x, screen_y, obj_px);
    color = BrightnessScale(composed, brightness_);
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
      // VRAM write may touch any BG's tilemap or tile data — conservatively
      // dirty every BG's row cache so the next fetch re-reads from VRAM.
      bg_row_dirty_.fill(true);
      break;
    }
    case sppu::regs::kVmDataH: {
      const uint32_t word_addr_shifted = static_cast<uint32_t>(TranslateVramAddress(vmadd_)) << 1U;
      const uint16_t byte_addr = static_cast<uint16_t>(word_addr_shifted | 1U);
      (*vram_)[byte_addr] = data;
      MaybeIncrementVmaddOnPort(/*is_high_port=*/true);
      bg_row_dirty_.fill(true);
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
      // Size pair / tile-region change affects every cached entry's width,
      // height, and tile mapping. Force the next OBJ fetch to re-evaluate.
      obj_line_v_ = -1;
      break;
    }

    case sppu::regs::kBgmode:
      bg_mode_ = data & sppu::regs::kBgmodeModeMask;
      bg3_priority_ = (data & sppu::regs::kBgmodeBg3PriorityMask) != 0U;
      bg_tile_16x16_[0] = (data & sppu::regs::kBgmodeBg1TileSizeMask) != 0U;
      bg_tile_16x16_[1] = (data & sppu::regs::kBgmodeBg2TileSizeMask) != 0U;
      bg_tile_16x16_[2] = (data & sppu::regs::kBgmodeBg3TileSizeMask) != 0U;
      bg_tile_16x16_[3] = (data & sppu::regs::kBgmodeBg4TileSizeMask) != 0U;
      // Tile-size flip changes tile_w and the bytes_per_char ladder; dirty all.
      bg_row_dirty_.fill(true);
      break;

    case sppu::regs::kBg1Sc:
    case sppu::regs::kBg2Sc:
    case sppu::regs::kBg3Sc:
    case sppu::regs::kBg4Sc: {
      const std::size_t bg = static_cast<std::size_t>(offset - sppu::regs::kBg1Sc);
      // Bits 7:2 are the screen base in 1K-word steps → word base = bits<<10.
      bg_tilemap_word_base_[bg] = static_cast<uint16_t>(static_cast<uint16_t>(data & sppu::regs::kBgScBaseMask) << 8);
      bg_tilemap_layout_[bg] = static_cast<uint8_t>(data & sppu::regs::kBgScLayoutMask);
      bg_row_dirty_[bg] = true;
      break;
    }

    case sppu::regs::kBg12Nba: WriteBgCharBase(0U, data); break;
    case sppu::regs::kBg34Nba: WriteBgCharBase(2U, data); break;

    case sppu::regs::kBg1Hofs:
    case sppu::regs::kBg2Hofs:
    case sppu::regs::kBg3Hofs:
    case sppu::regs::kBg4Hofs:
      WriteBgScroll(static_cast<uint8_t>((offset - sppu::regs::kBg1Hofs) >> 1U), data, /*is_hofs=*/true);
      break;
    case sppu::regs::kBg1Vofs:
    case sppu::regs::kBg2Vofs:
    case sppu::regs::kBg3Vofs:
    case sppu::regs::kBg4Vofs:
      WriteBgScroll(static_cast<uint8_t>((offset - sppu::regs::kBg1Vofs) >> 1U), data, /*is_hofs=*/false);
      break;

    case sppu::regs::kTm: main_screen_layers_ = data; break;
    case sppu::regs::kTs: sub_screen_layers_ = data; break;

    case sppu::regs::kCgwsel: cgwsel_ = data; break;
    case sppu::regs::kCgadsub: cgadsub_ = data; break;
    case sppu::regs::kColdata: {
      // Each write may set R, G, B independently (bits 5, 6, 7). Bits 4..0
      // hold the 5-bit intensity applied to whichever channels are selected.
      // Channels not selected retain their previous latch value.
      const uint8_t intensity = static_cast<uint8_t>(data & sppu::regs::kColdataIntensityMask);
      if ((data & sppu::regs::kColdataApplyRedMask) != 0U) coldata_r_ = intensity;
      if ((data & sppu::regs::kColdataApplyGreenMask) != 0U) coldata_g_ = intensity;
      if ((data & sppu::regs::kColdataApplyBlueMask) != 0U) coldata_b_ = intensity;
      break;
    }

    default:
      // Every other $2100-$213F write is shadow-only in v1. The shadow was
      // already updated at enqueue time, so nothing to do here.
      break;
  }
}

uint16_t Ppu::TranslateVramAddress(uint16_t raw) const {
  // VMAIN bits 3:2 choose one of four address rotations (fullsnes "F" field):
  //   00: no translation
  //   01: rotate low 8 bits:  aaaaaaaa YYYxxxxx  ->  aaaaaaaa xxxxxYYY
  //   10: rotate low 9 bits:  aaaaaaa YYYxxxxxx  ->  aaaaaaa xxxxxxYYY
  //   11: rotate low 10 bits: aaaaaa YYYxxxxxxx  ->  aaaaaa xxxxxxxYYY
  // Each rotation moves the top 3 bits of an n-bit field (YYY) to the bottom.
  const uint8_t mode =
      static_cast<uint8_t>((vmain_ & sppu::regs::kVmainTranslateMask) >> sppu::regs::kVmainTranslateShift);
  if (mode == 0U) return raw;
  static constexpr std::array<uint8_t, 4> kRotateWidth = {0U, 8U, 9U, 10U};
  const uint8_t n = kRotateWidth[mode];
  const uint8_t shift = static_cast<uint8_t>(n - 3U);
  const uint16_t high = static_cast<uint16_t>(raw & static_cast<uint16_t>(~((1U << n) - 1U)));
  const uint16_t yyy = static_cast<uint16_t>((raw >> shift) & 0x07U);
  const uint16_t xxx = static_cast<uint16_t>(raw & ((1U << shift) - 1U));
  return static_cast<uint16_t>(high | (xxx << 3U) | yyy);
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

void Ppu::MaybeAdvanceVramReadOnPort(bool is_high_port) {
  // RDVRAML/H reload the prefetch latch from the *current* vmadd_ and then
  // advance vmadd_ — same gating as the write-side increment.
  const bool increment_on_high = (vmain_ & sppu::regs::kVmainIncrementOnHighMask) != 0U;
  if (is_high_port == increment_on_high) {
    PrefetchVram();
    vmadd_ = static_cast<uint16_t>(vmadd_ + VmainIncrementStep());
  }
}

MmioReadResult Ppu::ReadOpct(uint16_t counter, bool& read_high) {
  // Per-register read-twice flipflop. 1st read returns bits 7:0 of the 9-bit
  // latched counter; 2nd read returns bit 8 (only bit 0 driven — bits 7:1 fall
  // to open-bus per fullsnes).
  if (!read_high) {
    read_high = true;
    return {static_cast<uint8_t>(counter & 0xFFU), 0xFFU};
  }
  read_high = false;
  return {static_cast<uint8_t>((counter >> 8) & 0x01U), sppu::regs::kOpctHighDrivenMask};
}

void Ppu::WriteBgCharBase(uint8_t bg_pair_base, uint8_t data) {
  // Low nibble = first BG, high nibble = second BG. Each nibble × 0x1000 word
  // steps. Used for both BG12NBA (bg_pair_base=0) and BG34NBA (bg_pair_base=2).
  bg_char_word_base_[bg_pair_base + 0U] = static_cast<uint16_t>(static_cast<uint16_t>(data & 0x0FU) << 12);
  bg_char_word_base_[bg_pair_base + 1U] = static_cast<uint16_t>(static_cast<uint16_t>((data >> 4) & 0x0FU) << 12);
  bg_row_dirty_[bg_pair_base + 0U] = true;
  bg_row_dirty_[bg_pair_base + 1U] = true;
}

void Ppu::WriteBgScroll(uint8_t bg, uint8_t data, bool is_hofs) {
  // Shared latch update. Per fullsnes:
  //   BGnHOFS = (Curr<<8) | (Prev & ~7) | ((Reg_old>>8) & 7)
  //   BGnVOFS = (Curr<<8) | Prev
  // HOFS: store the full 16-bit shift-in unmasked — the feedback term reads
  // the previous register's high byte (bits 15..8), which carries the prior
  // "Curr" verbatim. Masking to 10 bits at storage would drop bit 10 (== Curr
  // bit 2) and clear bit 2 of every smooth scroll step, producing visible
  // 4-pixel jitter. Render path masks with kBgScrollMask.
  // VOFS: no feedback term; mask to 10 bits at storage.
  // V scroll changes pixel_in_y → dirty the cache. H scroll intentionally
  // doesn't: the (eff_x >> 3) cache key re-keys naturally at column crossings.
  const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(data) << 8);
  if (is_hofs) {
    const uint16_t mid = static_cast<uint16_t>(bg_scroll_prev_ & 0xF8U);
    const uint16_t low = static_cast<uint16_t>((bg_hofs_[bg] >> 8) & 0x07U);
    bg_hofs_[bg] = static_cast<uint16_t>(high | mid | low);
  } else {
    const uint16_t low = static_cast<uint16_t>(bg_scroll_prev_);
    bg_vofs_[bg] = static_cast<uint16_t>((high | low) & sppu::regs::kBgScrollMask);
    bg_row_dirty_[bg] = true;
  }
  bg_scroll_prev_ = data;
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
  if (bg >= sppu::regs::kBgCount) {
    return {0U, true, false};
  }
  const bool bg_is_2bpp = (bpp == 2U);

  const uint32_t tile_w = bg_tile_16x16_[bg] ? 16U : 8U;
  const uint32_t tile_h = tile_w;  // SNES BG tiles are square.

  const uint32_t eff_x = (screen_x + bg_hofs_[bg]) & 0x3FFU;  // 10-bit wrap (covers 64-tile width).
  const int32_t cache_key = static_cast<int32_t>(eff_x >> 3U);

  BgRowCache& cache = bg_row_cache_[bg];
  if (cache.key != cache_key || bg_row_dirty_[bg]) {
    // Cache miss: decode the tilemap entry and load the 8-pixel column's
    // plane bytes. This is the heavy path FetchBgPixel used to walk on every
    // dot; under the cache it runs at most once per 8 pixels.
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

    uint32_t pixel_in_y = eff_y % tile_h;
    if (vflip) pixel_in_y = (tile_h - 1U) - pixel_in_y;

    // 16x16: choose the correct 8x8 sub-tile. The cache key advances every 8
    // logical pixels so sub_x flips between 0 and 1 inside a 16x16 tile. With
    // hflip the sub-tile order on the wire reverses.
    if (tile_w == 16U) {
      const uint32_t sub_x_logical = (eff_x >> 3U) & 1U;
      const uint32_t sub_x = hflip ? (1U - sub_x_logical) : sub_x_logical;
      const uint32_t sub_y = pixel_in_y >> 3U;
      char_index = static_cast<uint16_t>(char_index + sub_x + (sub_y << 4U));
      pixel_in_y &= 7U;
    }

    const uint32_t bytes_per_char = bg_is_2bpp ? 16U : 32U;
    const uint32_t char_byte_base = static_cast<uint32_t>(bg_char_word_base_[bg]) << 1U;
    const uint32_t tile_byte_addr =
        char_byte_base + (static_cast<uint32_t>(char_index) * bytes_per_char) + (pixel_in_y * 2U);

    cache.plane0 = GetVramByte(tile_byte_addr + 0U);
    cache.plane1 = GetVramByte(tile_byte_addr + 1U);
    if (bg_is_2bpp) {
      cache.plane2 = 0;
      cache.plane3 = 0;
    } else {
      cache.plane2 = GetVramByte(tile_byte_addr + 16U);
      cache.plane3 = GetVramByte(tile_byte_addr + 17U);
    }
    cache.palette_group = palette_group;
    cache.priority = priority;
    cache.hflip = hflip;
    cache.is_2bpp = bg_is_2bpp;
    cache.key = cache_key;
    bg_row_dirty_[bg] = false;
  }

  // Pixel extract — same bit-shift logic as the uncached version, but reading
  // the four cached plane bytes instead of VRAM directly.
  uint32_t bit_in_x = eff_x & 7U;
  if (cache.hflip) bit_in_x = 7U - bit_in_x;
  const uint8_t shift = static_cast<uint8_t>(7U - bit_in_x);
  const uint8_t p0 = (cache.plane0 >> shift) & 1U;
  const uint8_t p1 = (cache.plane1 >> shift) & 1U;
  uint8_t color_index = static_cast<uint8_t>(p0 | (p1 << 1U));
  if (!cache.is_2bpp) {
    const uint8_t p2 = (cache.plane2 >> shift) & 1U;
    const uint8_t p3 = (cache.plane3 >> shift) & 1U;
    color_index = static_cast<uint8_t>(color_index | (p2 << 2U) | (p3 << 3U));
  }

  if (color_index == 0U) {
    return {0U, true, cache.priority};
  }
  const uint8_t cgram_index = static_cast<uint8_t>((static_cast<uint8_t>(cache.palette_group) << bpp) | color_index);
  return {cgram_index, false, cache.priority};
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

void Ppu::EvaluateObjLine(uint32_t screen_y) const {
  obj_line_count_ = 0;
  obj_line_v_ = static_cast<int32_t>(screen_y);

  const ObjSizePair sizes = kObjSizes[obj_size_select_];

  for (uint8_t obj = 0; obj < 128U; ++obj) {
    const uint16_t low_addr = static_cast<uint16_t>(obj * 4U);

    // High-table byte holds (X-high, size) bit-pair at position (obj%4)*2 for
    // four OBJs at a time. Read it + Y first so the common "no Y overlap"
    // reject path skips the X/tile/attr reads.
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
    int16_t signed_x = static_cast<int16_t>(x_lo);
    if (x_high_bit) {
      signed_x = static_cast<int16_t>(signed_x | static_cast<int16_t>(0xFF00));
    }

    const uint8_t tile_lo = ReadOamByte(static_cast<uint16_t>(low_addr + 2U));
    const uint8_t attr = ReadOamByte(static_cast<uint16_t>(low_addr + 3U));
    const uint16_t base_tile = static_cast<uint16_t>(tile_lo | ((attr & sppu::regs::kObjAttrTileHighMask) << 8U));

    if (obj_line_count_ < kObjLineCap) {
      obj_line_list_[obj_line_count_++] = {signed_x, y_internal_8, width, height, base_tile, attr};
    }
    // OBJs past the cap are dropped (hardware time-over). The STAT77 bit isn't
    // surfaced yet (see ReadRegister stub), but the visible-pixel behavior
    // matches: lowest OAM indices win, anything past 32 doesn't render.
  }
}

Ppu::ObjPixel Ppu::FetchObjPixel(uint32_t screen_x, uint32_t screen_y) const {
  // Lazy-build the per-line sprite list the first time a new scanline asks
  // for an OBJ pixel. Once latched, mid-line OAM writes don't perturb the
  // current line — matching hardware where OAM evaluation runs in the tail
  // of the previous scanline.
  if (obj_line_v_ != static_cast<int32_t>(screen_y)) {
    EvaluateObjLine(screen_y);
  }

  for (uint8_t i = 0; i < obj_line_count_; ++i) {
    const ObjLineEntry& e = obj_line_list_[i];

    const int32_t x_internal = static_cast<int32_t>(screen_x) - static_cast<int32_t>(e.x);
    if (x_internal < 0 || x_internal >= static_cast<int32_t>(e.width)) continue;

    const bool hflip = (e.attr & sppu::regs::kObjAttrHflipMask) != 0U;
    const bool vflip = (e.attr & sppu::regs::kObjAttrVflipMask) != 0U;
    uint32_t pix_x = static_cast<uint32_t>(x_internal);
    uint32_t pix_y = static_cast<uint32_t>(e.y_internal);
    if (hflip) pix_x = (static_cast<uint32_t>(e.width) - 1U) - pix_x;
    if (vflip) pix_y = (static_cast<uint32_t>(e.height) - 1U) - pix_y;

    const uint32_t sub_x = pix_x >> 3U;
    const uint32_t sub_y = pix_y >> 3U;
    const uint32_t in_x = pix_x & 7U;
    const uint32_t in_y = pix_y & 7U;

    // 9-bit tile number (bit 8 from attr.0). Low nibble wraps within its
    // 16-tile row; high nibble wraps within its 16-row page; the region-select
    // bit does NOT carry.
    const uint16_t tile_x_low = static_cast<uint16_t>(((e.base_tile & 0x0FU) + sub_x) & 0x0FU);
    const uint16_t tile_y_low = static_cast<uint16_t>((((e.base_tile >> 4U) & 0x0FU) + sub_y) & 0x0FU);
    const uint16_t region_bit = static_cast<uint16_t>(e.base_tile & 0x100U);
    const uint16_t effective_tile = static_cast<uint16_t>(region_bit | (tile_y_low << 4U) | tile_x_low);

    const uint16_t region_word_base = (effective_tile & 0x100U) ? obj_region1_word_ : obj_region0_word_;
    const uint16_t tile_in_region = static_cast<uint16_t>(effective_tile & 0xFFU);
    const uint32_t tile_byte_addr =
        (static_cast<uint32_t>(region_word_base) << 1U) + (static_cast<uint32_t>(tile_in_region) * 32U) + (in_y * 2U);

    const uint8_t shift = static_cast<uint8_t>(7U - in_x);
    const uint8_t p0 = (GetVramByte(tile_byte_addr + 0U) >> shift) & 1U;
    const uint8_t p1 = (GetVramByte(tile_byte_addr + 1U) >> shift) & 1U;
    const uint8_t p2 = (GetVramByte(tile_byte_addr + 16U) >> shift) & 1U;
    const uint8_t p3 = (GetVramByte(tile_byte_addr + 17U) >> shift) & 1U;
    const uint8_t color_index = static_cast<uint8_t>(p0 | (p1 << 1U) | (p2 << 2U) | (p3 << 3U));
    if (color_index == 0U) {
      continue;
    }

    // OBJ palette region: $80..$FF, eight 16-color groups.
    const unsigned palette_group =
        (static_cast<unsigned>(e.attr) >> sppu::regs::kObjAttrPaletteShift) & sppu::regs::kObjAttrPaletteMask;
    const uint8_t cgram_index =
        static_cast<uint8_t>(0x80U | (palette_group << 4U) | static_cast<unsigned>(color_index));
    const uint8_t priority = static_cast<uint8_t>((static_cast<unsigned>(e.attr) >> sppu::regs::kObjAttrPriorityShift) &
                                                  sppu::regs::kObjAttrPriorityMask);
    return {cgram_index, false, priority};
  }

  return {0U, true, 0U};
}

}  // namespace pupsnes
