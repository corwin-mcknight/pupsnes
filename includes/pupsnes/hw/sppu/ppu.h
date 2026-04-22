#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "pupsnes/hw/device.h"
#include "pupsnes/hw/sppu/ppu_regs.h"

namespace pupsnes {

class SystemBus;

// View over one of the PPU's two framebuffers, handed to the frontend via the
// SNES::FireFrameReady callback. Pixel stride is the full H-grid width; the
// consumer typically only reads the first `width` pixels per row (the current
// logical mode width, e.g. 256 for Mode 0-6 / 512 for mode 5/6). `height`
// reflects the current overscan/interlace state.
struct FrameBufferView {
  const uint16_t* pixels;  // BGR555
  uint32_t width;
  uint32_t height;
  uint32_t stride;  // Distance between adjacent rows in pixels.
};

// Entry in the PPU's pending-write log. Same-clock MMIO writes don't trigger
// catch-up; they append here and the dot loop replays them in order at the
// cycle they arrived. See `ppu_scaffold_design.md` (memory) for rationale.
struct PpuPokeLogEntry {
  TimeMasterT cycle;
  uint16_t offset;  // Full 24-bit device offset; only bits [7:0] map to $21xx.
  uint8_t data;
};

// SPPU — Super Nintendo Picture Processing Unit (v1 scaffold).
//
// v1 scope (backdrop-only rendering):
//   * Own the B-bus window at $2100-$213F, mapped across banks $00-$3F / $80-$BF.
//   * Advance through time with a dot-major Tick (populated in Phase D).
//   * Maintain VRAM/OAM/CGRAM backing plus real port protocols (write-twice,
//     auto-increment, read-prefetch) so Phase-C ports behave correctly before
//     the renderer catches up in later milestones.
//   * Emit one backdrop pixel per visible dot (`cgram[0]` × INIDISP brightness,
//     or 0 under forced-blank).
//
// Out of scope for v1: BG/OBJ/windows/mode-7, NMI, HIRQ/VIRQ, color math,
// H/V-counter latching, state-block migration.
//
// Synchronization model — "writes queue, reads catch up":
//   * WriteRegister appends to `pending_writes_` and returns immediately.
//   * ReadRegister runs after the bus has already caught us up to the read's
//     master cycle (SystemBus gates `kSameClockMmio` catch-up on reads), so
//     the pending log is drained through the caller's Tick before the read
//     observes decoded state.
class Ppu : public Device {
 public:
  explicit Ppu(SNES* snes);
  ~Ppu() override = default;

  // Bus wiring — claim the entire page $21 as kSameClockMmio in banks $00-$3F
  // and $80-$BF. APU ports at $2140-$21FF are absorbed by the PPU today
  // (reads return open-bus, writes drop) and migrate to the APU device once
  // it lands.
  void MapSystemBus(SystemBus& bus);

  // Drop all PPU state back to power-on defaults. Does not re-map the bus;
  // callers run MapSystemBus before the first Reset. Future Phase D will also
  // prime the first scanline-end scheduler event from here.
  void Reset();

  [[nodiscard]] TickResult Tick(TimeMasterDeltaT budget) override;
  void OnEvent(const SchedulerEvent& event) override;
  [[nodiscard]] MmioReadResult ReadRegister(uint32_t offset, TimeMasterT current_time) override;
  void WriteRegister(uint32_t offset, uint8_t data, TimeMasterT current_time) override;

  // Buffer accessors — both buffers are always readable so the debugger can
  // sample the in-progress frame without waiting for a swap. Buffers are sized
  // for the full 341 × 313 H/V grid and store BGR555 uint16_t pixels.
  [[nodiscard]] const uint16_t* GetFrontBuffer() const { return front_buffer_->data(); }
  [[nodiscard]] const uint16_t* GetBackBuffer() const { return back_buffer_->data(); }

  // Construct a view over the current front buffer using the dynamic logical
  // dimensions (256 × 224/239 today; wider when interlace/hires land later).
  [[nodiscard]] FrameBufferView BuildFrontView() const;

  // Debugger / test accessors.
  [[nodiscard]] uint8_t GetShadow(uint16_t reg) const {
    return shadow_[static_cast<std::size_t>(reg - sppu::regs::kBase) & (sppu::regs::kShadowSize - 1U)];
  }
  [[nodiscard]] bool IsForcedBlank() const { return forced_blank_; }
  [[nodiscard]] uint8_t GetBrightness() const { return brightness_; }
  [[nodiscard]] bool IsOverscan() const { return overscan_; }
  [[nodiscard]] uint32_t GetPendingWriteCount() const { return pending_writes_count_ - pending_writes_cursor_; }
  void SetForceOverscanDraw(bool v) { force_overscan_draw_ = v; }
  [[nodiscard]] bool GetForceOverscanDraw() const { return force_overscan_draw_; }
  [[nodiscard]] uint32_t GetDotH() const { return h_; }
  [[nodiscard]] uint32_t GetDotV() const { return v_; }
  [[nodiscard]] bool GetField() const { return field_; }

  // Pure timing helpers — exposed for tests and the debugger time display.
  // NTSC non-interlace only in v1; PAL / interlaced long-line support lands
  // with the timing overhaul later.
  [[nodiscard]] static constexpr bool IsShortLine(uint32_t v, bool field) {
    // fullsnes: "On NTSC non-interlaced mode, V=240 becomes 1360 cycles on
    // every other frame." We flag the `field_=true` frame as the short one.
    return v == 240U && field;
  }
  [[nodiscard]] static constexpr TimeMasterDeltaT DotCost(uint32_t h, uint32_t v, bool field) {
    if (h == 323U || h == 327U) {
      return IsShortLine(v, field) ? 4 : 6;
    }
    return 4;
  }
  [[nodiscard]] static constexpr TimeMasterDeltaT LineCycles(uint32_t v, bool field) {
    return IsShortLine(v, field) ? sppu::regs::kShortLineCycles : sppu::regs::kNormalLineCycles;
  }
  // Apply INIDISP brightness to a BGR555 colour, per fullsnes:
  //   out_channel = (channel * (brightness + 1)) >> 4
  // Brightness 0 collapses to ~1/16th; brightness 15 passes through unchanged.
  [[nodiscard]] static constexpr uint16_t BrightnessScale(uint16_t bgr555, uint8_t brightness) {
    const uint32_t r = bgr555 & 0x1FU;
    const uint32_t g = (static_cast<uint32_t>(bgr555) >> 5U) & 0x1FU;
    const uint32_t b = (static_cast<uint32_t>(bgr555) >> 10U) & 0x1FU;
    const uint32_t factor = static_cast<uint32_t>(brightness) + 1U;
    const uint32_t r_out = (r * factor) >> 4U;
    const uint32_t g_out = (g * factor) >> 4U;
    const uint32_t b_out = (b * factor) >> 4U;
    return static_cast<uint16_t>(r_out | (g_out << 5U) | (b_out << 10U));
  }

 private:
  // Append a same-clock MMIO write to the pending log. Returns true on
  // success, false when the log is full (caller triggers an internal flush via
  // Tick in that case; Phase D fills this in).
  bool EnqueueWrite(uint16_t offset, uint8_t data, TimeMasterT cycle);

  // Drain the pending-write log up to `cutoff` (inclusive). Writes at later
  // cycles stay in the log for the next Tick iteration. Once the cursor
  // reaches the tail, the log is reset so subsequent enqueues start at index 0.
  void DrainPendingWritesUpTo(TimeMasterT cutoff);

  // Replay dispatcher — applies one pending write's semantics. Writes that
  // only touch the shadow (open-bus ports) are no-ops here because the shadow
  // was updated at enqueue time.
  void ReplayWrite(uint16_t offset, uint8_t data);

  // Dot-loop helpers.
  void AdvanceHv();
  void EmitPixel(uint32_t h, uint32_t v);
  // End-of-frame: swap the double buffers, fire the frontend callback with a
  // view over the (now-front) completed frame, and toggle `field_` for the
  // next frame's short-line selection.
  void OnEndOfFrame();

  // VRAM helpers. Address translation maps the raw vmadd_ to a rotated layout
  // per VMAIN bits 3:2; prefetch caches the translated word; increment fires
  // on the port matching VMAIN bit 7.
  [[nodiscard]] uint16_t TranslateVramAddress(uint16_t raw) const;
  [[nodiscard]] uint16_t VmainIncrementStep() const;
  void PrefetchVram();
  void MaybeIncrementVmaddOnPort(bool is_high_port);

  // OAM helpers. Handles the byte-address wrap for the 32-byte high table
  // mirroring above $220; writes and reads all go through these.
  void WriteOamByte(uint16_t byte_addr, uint8_t data);
  [[nodiscard]] uint8_t ReadOamByte(uint16_t byte_addr) const;

  // --- Register shadow + decoded fields ---
  std::array<uint8_t, sppu::regs::kShadowSize> shadow_{};

  // Decoded INIDISP ($2100) — updated during log replay.
  uint8_t inidisp_ = 0x80;   // power-on: forced blank set.
  bool forced_blank_ = true;
  uint8_t brightness_ = 0x0F;

  // Decoded SETINI ($2133).
  bool overscan_ = false;
  // Emulator-only override forcing the view to report 239 rows even when the
  // ROM left SETINI.bit2 clear. Useful for inspecting the overscan region in
  // the debugger; does not affect the underlying framebuffer data.
  bool force_overscan_draw_ = false;

  // CGRAM addressing + latches. Write-twice latch and read-twice latch track
  // independently; writing $2121 (CGADD) clears both (the byte address becomes
  // word-aligned again). cgadd_ is the 8-bit word index into cgram_.
  uint8_t cgadd_ = 0;
  uint8_t cgram_write_latch_data_ = 0;
  bool cgram_write_latch_high_ = false;
  uint8_t cgram_read_latch_data_ = 0;
  bool cgram_read_latch_high_ = false;

  // VRAM addressing. vmadd_ is the 16-bit word address (only 15 bits reach the
  // 64 KiB backing store; top bit is unused). vram_prefetch_ caches the 16-bit
  // word at translated(vmadd_), loaded by VMADDL/VMADDH writes and refreshed
  // by VMDATA reads per VMAIN bit 7.
  uint16_t vmadd_ = 0;
  uint8_t vmain_ = 0;
  uint16_t vram_prefetch_ = 0;

  // OAM addressing. Live state is a 10-bit byte address; $2102/$2103 writes
  // update both the live pointer and the reload latch that Phase D will copy
  // back at start-of-frame (or during forced blank).
  uint16_t oam_byte_addr_ = 0;
  uint16_t oam_byte_addr_reload_ = 0;
  bool oam_priority_rotation_ = false;
  uint8_t oam_write_latch_ = 0;  // Low-byte latch for write-twice in low OAM.

  // Dot/scanline position + field toggle.
  uint32_t h_ = 0;
  uint32_t v_ = 0;
  bool field_ = false;
  // Cycles already consumed toward the current dot from prior Tick calls.
  // Range: [0, DotCost(h_, v_, field_)). Lets one dot span multiple Ticks
  // when budget lands mid-dot — no budget overshoot permitted.
  TimeMasterDeltaT partial_dot_cycles_ = 0;

  // --- Backing storage ---
  // Heap-allocated via unique_ptr<array> to keep the parent SNES object small
  // on the stack (64K VRAM + two 213KB framebuffers would otherwise blow it).
  std::unique_ptr<std::array<uint8_t, sppu::regs::kVramSize>> vram_;
  std::unique_ptr<std::array<uint8_t, sppu::regs::kOamSize>> oam_;
  std::unique_ptr<std::array<uint16_t, sppu::regs::kCgramWords>> cgram_;

  std::unique_ptr<std::array<uint16_t, sppu::regs::kFrameBufferPixels>> front_buffer_;
  std::unique_ptr<std::array<uint16_t, sppu::regs::kFrameBufferPixels>> back_buffer_;

  // --- Pending-write log ---
  // Fixed-size to avoid allocation on the hot write path. DMA bursts and
  // HDMA can enqueue thousands of entries per frame; 16K covers the worst
  // case. Overflow triggers an internal flush via Tick (Phase D).
  static constexpr std::size_t kPendingWriteLogSize = 16384;
  std::unique_ptr<std::array<PpuPokeLogEntry, kPendingWriteLogSize>> pending_writes_;
  uint32_t pending_writes_count_ = 0;
  // Position within pending_writes_ of the next entry to replay. Writes with
  // a cycle above the current drain cutoff sit here until a later Tick picks
  // them up. Collapses to 0 once the tail is reached.
  uint32_t pending_writes_cursor_ = 0;
};

}  // namespace pupsnes
