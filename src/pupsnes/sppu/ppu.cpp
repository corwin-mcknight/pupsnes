#include "pupsnes/hw/sppu/ppu.h"

#include <algorithm>
#include <memory>

#include "pupsnes/hw/scheduler.h"
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

}  // namespace

Ppu::Ppu(SNES* snes)
    : Device(snes),
      vram_(std::make_unique<std::array<uint8_t, sppu::regs::kVramSize>>()),
      oam_(std::make_unique<std::array<uint8_t, sppu::regs::kOamSize>>()),
      cgram_(std::make_unique<std::array<uint16_t, sppu::regs::kCgramWords>>()),
      front_buffer_(std::make_unique<std::array<uint16_t, sppu::regs::kFrameBufferPixels>>()),
      back_buffer_(std::make_unique<std::array<uint16_t, sppu::regs::kFrameBufferPixels>>()),
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
  inidisp_ = 0x80U;
  forced_blank_ = true;
  brightness_ = 0;
  overscan_ = false;

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

  vram_->fill(0);
  oam_->fill(0);
  cgram_->fill(0);
  front_buffer_->fill(0);
  back_buffer_->fill(0);

  // NOTE: Reset intentionally does not auto-schedule a first kDeviceRun.
  // Pre-scheduling at an arbitrary future time collides with other devices'
  // micro-op retirement (kMaxMicroOpOvershoot) pushing master_time past the
  // PPU event's fire time. The emulator main loop (or integration tests)
  // drives the PPU explicitly via Scheduler::CatchUpDevice / ScheduleDeviceRun;
  // bus reads of PPU registers also catch the PPU up through the same path.
}

TickResult Ppu::Tick(TimeMasterDeltaT budget) {
  // Sub-dot advancement: a dot is 4-6 mcyc but the scheduler may hand us a
  // smaller budget mid-dot. Track how many cycles of the current dot we've
  // already consumed in prior Tick calls so we can split one dot across
  // multiple Ticks without ever exceeding the budget. Consumed cycles are
  // strictly <= budget — no overshoot.
  TimeMasterDeltaT consumed = 0;
  while (consumed < budget) {
    const TimeMasterDeltaT dot_cost = DotCost(h_, v_, field_);
    const TimeMasterDeltaT remaining_dot = dot_cost - partial_dot_cycles_;
    const TimeMasterDeltaT available = budget - consumed;

    if (available < remaining_dot) {
      // Budget runs out mid-dot. Bank the partial progress on the PPU side
      // (via partial_dot_cycles_) and return. No pixel emit yet — the dot
      // emits when its final cycle completes in a later Tick.
      partial_dot_cycles_ += available;
      consumed += available;
      return {consumed, TickStopReason::kBudgetExhausted};
    }

    // Enough budget to finish the current dot. Drain writes up to the dot's
    // nominal start cycle so EmitPixel observes the state that was valid
    // when this dot began.
    const TimeMasterT dot_start_time =
        local_time_ + consumed - static_cast<TimeMasterDeltaT>(partial_dot_cycles_);
    DrainPendingWritesUpTo(dot_start_time);
    EmitPixel(h_, v_);
    AdvanceHv();
    consumed += remaining_dot;
    partial_dot_cycles_ = 0;

    if (h_ == 0) {
      // Scanline just ended. If V also wrapped, fire the frame callback
      // before the scheduler re-dispatches us.
      if (v_ == 0) {
        OnEndOfFrame();
        field_ = !field_;
      }
      // next_wake == committed_time — the scheduler's AlignDeviceTime
      // becomes a no-op and we don't gap-advance past pixels we haven't
      // emitted. HandleRunResult will schedule the next kDeviceRun at this
      // same time, and that run covers the next scanline.
      const TimeMasterT next_wake = local_time_ + consumed;
      return {consumed, TickStopReason::kReachedLocalBoundary, 0, next_wake};
    }
  }
  // Budget exactly consumed on a dot boundary or mid-dot (partial banked).
  return {consumed, TickStopReason::kBudgetExhausted};
}

void Ppu::OnEvent(const SchedulerEvent& /*event*/) {
  // Phase D populates this with scanline-end handling (swap buffers at V=261,
  // recompute next wake time). Phase B/C ignore scheduler events.
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
    // $2140-$21FF: APU / WRAM port range, open-bus until the APU device
    // takes ownership.
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
      const uint8_t driven =
          static_cast<uint8_t>(sppu::regs::kStat78VersionMask | sppu::regs::kStat78PalMask | sppu::regs::kStat78FieldMask);
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
    // Out-of-range writes within the PPU page are dropped. APU / WRAM-port
    // writes land here until those devices claim the tail of page $21.
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
    // Phase D: on overflow, force an internal catch-up flush by calling the
    // replay loop here. For the scaffold we just drop the entry so we don't
    // corrupt the log; the scaffold has no live writer that can fill 16K
    // entries in a single bus transaction.
    return false;
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
  }
}

void Ppu::EmitPixel(uint32_t h, uint32_t v) {
  const bool in_visible_h = (h >= sppu::regs::kVisibleHStart && h < sppu::regs::kVisibleHEnd);
  const uint32_t v_end = (force_overscan_draw_ || overscan_) ? sppu::regs::kVisibleVEnd239 : sppu::regs::kVisibleVEnd224;
  const bool in_visible_v = (v >= sppu::regs::kVisibleVStartNtsc && v < v_end);

  uint16_t color = 0;
  if (in_visible_h && in_visible_v && !forced_blank_) {
    // v1 scaffold: every visible dot draws the backdrop (CGRAM[0]) scaled by
    // INIDISP brightness. BG / OBJ / window compositing lands with later
    // milestones.
    color = BrightnessScale((*cgram_)[0], brightness_);
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
      inidisp_ = data;
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

    case sppu::regs::kVmain:
      vmain_ = data;
      break;
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
        const uint16_t word = static_cast<uint16_t>(
            (static_cast<uint16_t>(data & 0x7FU) << 8) | cgram_write_latch_data_);
        (*cgram_)[cgadd_] = word;
        cgadd_ = static_cast<uint8_t>(cgadd_ + 1U);
        cgram_write_latch_high_ = false;
      }
      break;

    case sppu::regs::kSetini:
      overscan_ = (data & sppu::regs::kSetiniOverscanMask) != 0U;
      break;

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
  const uint8_t mode = static_cast<uint8_t>((vmain_ & sppu::regs::kVmainTranslateMask) >> sppu::regs::kVmainTranslateShift);
  switch (mode) {
    case 0:
      return raw;
    case 1: {
      const uint16_t high = static_cast<uint16_t>(raw & 0xFF00U);
      const uint16_t rotated =
          static_cast<uint16_t>(((raw & 0x00E0U) >> 5) | ((raw & 0x001FU) << 3));
      return static_cast<uint16_t>(high | rotated);
    }
    case 2: {
      const uint16_t high = static_cast<uint16_t>(raw & 0xFE00U);
      const uint16_t rotated =
          static_cast<uint16_t>(((raw & 0x01C0U) >> 6) | ((raw & 0x003FU) << 3));
      return static_cast<uint16_t>(high | rotated);
    }
    case 3: {
      const uint16_t high = static_cast<uint16_t>(raw & 0xFC00U);
      const uint16_t rotated =
          static_cast<uint16_t>(((raw & 0x0380U) >> 7) | ((raw & 0x007FU) << 3));
      return static_cast<uint16_t>(high | rotated);
    }
    default:
      return raw;
  }
}

uint16_t Ppu::VmainIncrementStep() const {
  switch (vmain_ & sppu::regs::kVmainStepMask) {
    case 0x00U: return 1;
    case 0x01U: return 32;
    case 0x02U:
    case 0x03U: return 128;
    default:    return 1;
  }
}

void Ppu::PrefetchVram() {
  const uint16_t byte_addr = static_cast<uint16_t>(TranslateVramAddress(vmadd_) << 1);
  const uint8_t lo = (*vram_)[byte_addr];
  const uint8_t hi = (*vram_)[static_cast<uint16_t>(byte_addr | 1U)];
  vram_prefetch_ = PackWord(lo, hi);
}

void Ppu::MaybeIncrementVmaddOnPort(bool is_high_port) {
  const bool increment_on_high = (vmain_ & sppu::regs::kVmainIncrementOnHighMask) != 0U;
  if (is_high_port == increment_on_high) {
    vmadd_ = static_cast<uint16_t>(vmadd_ + VmainIncrementStep());
  }
}

void Ppu::WriteOamByte(uint16_t byte_addr, uint8_t data) {
  (*oam_)[OamByteSlot(byte_addr)] = data;
}

uint8_t Ppu::ReadOamByte(uint16_t byte_addr) const {
  return (*oam_)[OamByteSlot(byte_addr)];
}

}  // namespace pupsnes
