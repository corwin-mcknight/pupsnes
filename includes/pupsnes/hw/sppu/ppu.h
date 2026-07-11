#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "pupsnes/core/device.h"
#include "pupsnes/core/emu_event.h"
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

// H/V-blank status bits surfaced to CPU-side consumers ($4212 HVBJOY today;
// IRQ/NMI wiring later). Computed lazily from the PPU's dot/scanline cursor
// after catching up to the caller's master time.
struct PpuHvbStatus {
  bool vblank;
  bool hblank;
};

// SPPU — Super Nintendo Picture Processing Unit. Owns the B-bus PPU window
// ($2100-$213F) mirrored across banks $00-$3F / $80-$BF, advances via a
// dot-major Tick, and renders Mode 1 per-dot (so per-line HDMA scroll
// modulation needs no extra plumbing — the dot loop drains pending writes up
// to the dot's start cycle before fetching).
//
// Synchronization model — "writes queue, reads catch up":
//   * WriteRegister appends to `pending_writes_` and returns immediately.
//   * ReadRegister runs after the bus has already caught us up to the read's
//     master cycle (SystemBus gates `kSameClockMmio` catch-up on reads), so
//     the pending log is drained through the caller's Tick before the read
//     observes decoded state.
class Ppu : public Device {
 public:
  explicit Ppu(SNES& snes);
  ~Ppu() override = default;

  [[nodiscard]] const char* DeviceName() const override { return "PPU"; }

  // Bus wiring — claim the entire page $21 as kSameClockMmio in banks $00-$3F
  // and $80-$BF. APU ports at $2140-$21FF are absorbed by the PPU today
  // (reads return open-bus, writes drop) and migrate to the APU device once
  // it lands.
  void MapSystemBus(SystemBus& bus);

  // Drop all PPU state back to power-on defaults. Does not re-map the bus;
  // callers run MapSystemBus before the first Reset.
  void Reset();

  void CatchUpTo(TimeMasterT target) override;
  [[nodiscard]] MmioReadResult ReadRegister(uint32_t offset, TimeMasterT current_time) override;
  void WriteRegister(uint32_t offset, uint8_t data, TimeMasterT current_time) override;

  // Catch up to `current_time` and report the H/V-blank flags the CPU-side
  // HVBJOY ($4212) register would see. Per fullsnes: VBlank spans V>=225 (or
  // V>=240 when SETINI overscan is enabled) through end of frame; HBlank
  // spans H>=274 through the start of the next scanline.
  [[nodiscard]] PpuHvbStatus QueryHvbStatus(TimeMasterT current_time);

  // Catch up to `current_time`, sample the VBlank-NMI latch, and clear it.
  // The latch sets when the PPU enters VBlank (V transitions to 225, or 240
  // under overscan) and clears on either a read or V rolling back to 0 at
  // frame start. Backs RDNMI ($4210) bit 7; BIT $4210 / BPL polling loops
  // see the latch go high once per frame and fall back to 0 after sampling.
  [[nodiscard]] bool QueryAndClearVblankNmiFlag(TimeMasterT current_time);
  // Read-only peek at the latch (no catch-up, no clear) — for the debugger.
  [[nodiscard]] bool PeekVblankNmiFlag() const { return vblank_nmi_flag_; }

  // Catch up to `current_time` and return the current level of the PPU /NMI
  // output pin. True while V is on the VBlank entry line (225 normally, 240
  // under overscan); false elsewhere. This is the continuous level signal
  // that goes through NMITIMEN.7 to the CPU's NMI edge detector — distinct
  // from the $4210 latch above. The CPU polls this at instruction-boundary
  // sample points and runs its own edge detection + gating.
  [[nodiscard]] bool SampleNmiLine(TimeMasterT current_time);
  // Read-only peek at the line level given already-current PPU state (no
  // catch-up). Used by the VBlank-NMI scheduler-fence handler, which runs
  // after MachineSync has already advanced the PPU.
  [[nodiscard]] bool PeekNmiLine() const;

  // Buffer accessors — both buffers are always readable so the debugger can
  // sample the in-progress frame without waiting for a swap. Buffers are sized
  // for the full 341 × 313 H/V grid and store BGR555 uint16_t pixels.
  [[nodiscard]] const uint16_t* GetFrontBuffer() const { return front_buffer_->data(); }
  [[nodiscard]] const uint16_t* GetBackBuffer() const { return back_buffer_->data(); }

  // Construct a view over the current front buffer using the dynamic logical
  // dimensions (256 × 224/239 today; wider when interlace/hires land later).
  [[nodiscard]] FrameBufferView BuildFrontView() const;

  // Drawn-mask accessor — one bit per framebuffer pixel, set when CatchUpTo
  // emits that pixel. Consumers (tests, PPU panel overlay) read this to know
  // which pixels in the back buffer are valid for the current in-progress frame.
  [[nodiscard]] const uint8_t* GetDrawnMask() const { return drawn_mask_->data(); }

  // Debugger / test accessors.
  [[nodiscard]] uint8_t GetShadow(uint16_t reg) const {
    return shadow_[static_cast<std::size_t>(reg - sppu::regs::kBase) & (sppu::regs::kShadowSize - 1U)];
  }
  [[nodiscard]] bool IsForcedBlank() const { return forced_blank_; }
  [[nodiscard]] uint8_t GetBrightness() const { return brightness_; }
  [[nodiscard]] bool IsOverscan() const { return overscan_; }
  // bg_hofs_ stores the raw 16-bit shift-in (see BG_old feedback term in
  // ReplayWrite). Mask to the 10-bit effective offset used by rendering.
  [[nodiscard]] uint16_t GetBgHofs(uint8_t bg) const {
    return bg < sppu::regs::kBgCount ? static_cast<uint16_t>(bg_hofs_[bg] & sppu::regs::kBgScrollMask) : uint16_t{0};
  }
  [[nodiscard]] uint16_t GetBgVofs(uint8_t bg) const { return bg < sppu::regs::kBgCount ? bg_vofs_[bg] : uint16_t{0}; }
  [[nodiscard]] uint32_t GetPendingWriteCount() const { return pending_writes_count_ - pending_writes_cursor_; }
  void SetForceOverscanDraw(bool v) { force_overscan_draw_ = v; }
  [[nodiscard]] bool GetForceOverscanDraw() const { return force_overscan_draw_; }
  [[nodiscard]] uint32_t GetDotH() const { return h_; }
  [[nodiscard]] uint32_t GetDotV() const { return v_; }
  [[nodiscard]] bool GetField() const { return field_; }
  [[nodiscard]] uint16_t GetOphct() const { return ophct_; }
  [[nodiscard]] uint16_t GetOpvct() const { return opvct_; }
  [[nodiscard]] bool GetHvLatchFlag() const { return hv_latch_flag_; }
  [[nodiscard]] uint8_t GetMainScreenLayers() const { return main_screen_layers_; }
  [[nodiscard]] uint8_t GetSubScreenLayers() const { return sub_screen_layers_; }
  [[nodiscard]] uint8_t GetCgwsel() const { return cgwsel_; }
  [[nodiscard]] uint8_t GetCgadsub() const { return cgadsub_; }
  // Live COLDATA latches — independent per channel; assembled to a BGR555
  // word at math time.
  [[nodiscard]] uint8_t GetColdataR() const { return coldata_r_; }
  [[nodiscard]] uint8_t GetColdataG() const { return coldata_g_; }
  [[nodiscard]] uint8_t GetColdataB() const { return coldata_b_; }
  [[nodiscard]] uint8_t GetBgMode() const { return bg_mode_; }
  [[nodiscard]] bool GetBg3Priority() const { return bg3_priority_; }
  [[nodiscard]] bool GetBgTile16x16(uint8_t bg) const { return bg < sppu::regs::kBgCount ? bg_tile_16x16_[bg] : false; }
  [[nodiscard]] uint8_t GetBgTilemapLayout(uint8_t bg) const {
    return bg < sppu::regs::kBgCount ? bg_tilemap_layout_[bg] : uint8_t{0};
  }
  [[nodiscard]] uint16_t GetBgTilemapWordBase(uint8_t bg) const {
    return bg < sppu::regs::kBgCount ? bg_tilemap_word_base_[bg] : uint16_t{0};
  }
  [[nodiscard]] uint16_t GetBgCharWordBase(uint8_t bg) const {
    return bg < sppu::regs::kBgCount ? bg_char_word_base_[bg] : uint16_t{0};
  }
  [[nodiscard]] uint8_t GetObjSizeSelect() const { return obj_size_select_; }
  [[nodiscard]] uint16_t GetObjRegion0Word() const { return obj_region0_word_; }
  [[nodiscard]] uint16_t GetObjRegion1Word() const { return obj_region1_word_; }
  [[nodiscard]] uint16_t GetVmadd() const { return vmadd_; }
  [[nodiscard]] uint8_t GetVmain() const { return vmain_; }
  [[nodiscard]] uint16_t GetVramPrefetch() const { return vram_prefetch_; }
  [[nodiscard]] uint16_t GetOamByteAddr() const { return oam_byte_addr_; }
  [[nodiscard]] uint16_t GetOamByteAddrReload() const { return oam_byte_addr_reload_; }
  [[nodiscard]] bool GetOamPriorityRotation() const { return oam_priority_rotation_; }
  [[nodiscard]] uint8_t GetCgadd() const { return cgadd_; }
  [[nodiscard]] bool GetCgramWriteLatchHigh() const { return cgram_write_latch_high_; }
  [[nodiscard]] uint8_t GetCgramWriteLatchData() const { return cgram_write_latch_data_; }
  [[nodiscard]] bool GetCgramReadLatchHigh() const { return cgram_read_latch_high_; }
  [[nodiscard]] const uint16_t* GetCgram() const { return cgram_->data(); }

  // Live cursor accessor used by the H/V-IRQ scheduler. Returns the current
  // (h, v, field) snapshot without catching the PPU up. Callers that need a
  // catch-up'd view should call CatchUpTo(now) first.
  struct Cursor {
    uint32_t h;
    uint32_t v;
    bool field;
    TimeMasterT local_time;
  };
  [[nodiscard]] Cursor GetCursor() const { return {h_, v_, field_, local_time_}; }

  // Master cycle of the start of dot (h, v) within the frame anchored at
  // master time `frame_origin` and field-toggle `field`. The frame origin is
  // the master cycle when (h=0, v=0) of that frame entered.
  //
  // Used by the H/V-IRQ scheduler in CpuMmio to compute the absolute master
  // cycle for a given (HTIME, VTIME) match. Does not advance PPU state.
  [[nodiscard]] static constexpr TimeMasterT MasterCycleAt(uint32_t target_h, uint32_t target_v,
                                                           TimeMasterT frame_origin, bool field) {
    TimeMasterT t = frame_origin;
    for (uint32_t v = 0; v < target_v; ++v) {
      t += LineCycles(v, field);
    }
    for (uint32_t h = 0; h < target_h; ++h) {
      t += DotCost(h, target_v, field);
    }
    return t;
  }

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

  // Project the H/V counters forward from the live cursor to master cycle
  // `t` — pure counter arithmetic over DotCost/field state with no machine
  // mutation. Used to stamp emitted EmuEvents with the counter position for
  // devices whose current time is ahead of the PPU's lazy catch-up (the PPU
  // never runs ahead of the machine, so the walk is forward-only; `t` at or
  // behind the in-progress dot returns that dot). The counters are stateful
  // on hardware — mode changes alter line/frame lengths — so events record
  // position at the source via this projection or the live cursor, never by
  // re-deriving from master_time later. H is the dot counter (0..339,
  // OPHCT semantics).
  [[nodiscard]] EmuEventHv ProjectHvAt(TimeMasterT t) const;
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
  // First V where VBlank is asserted — tracks SETINI overscan live.
  [[nodiscard]] uint32_t VblankStartLine() const {
    return overscan_ ? sppu::regs::kVisibleVEnd239 : sppu::regs::kVisibleVEnd224;
  }

  // Live counter position for stamping EmuEvents emitted during catch-up
  // (replay writes apply at the in-progress dot; the cursor IS the counter).
  [[nodiscard]] EmuEventHv CursorHv() const { return {static_cast<uint16_t>(v_), static_cast<uint16_t>(h_)}; }

  // Frame length in master cycles, accounting for the short-line saving at
  // V=240 on field=1 NTSC frames.
  [[nodiscard]] TimeMasterT NextFramePeriod() const {
    return static_cast<TimeMasterT>(sppu::regs::kLinesPerFrameNtsc) * sppu::regs::kNormalLineCycles -
           (field_ ? 4U : 0U);
  }

  // 64K-wrapped VRAM byte read; shared by FetchBgPixel and FetchObjPixel.
  [[nodiscard]] uint8_t GetVramByte(uint32_t addr) const { return (*vram_)[addr & 0xFFFFU]; }

  // Append a same-clock MMIO write to the pending log. Returns true on
  // success, false when the log is full (caller triggers an internal flush via
  // Tick in that case).
  bool EnqueueWrite(uint16_t offset, uint8_t data, TimeMasterT cycle);

  // Drain the pending-write log up to `cutoff` (inclusive). Writes at later
  // cycles stay in the log for the next Tick iteration. Once the cursor
  // reaches the tail, the log is reset so subsequent enqueues start at index 0.
  void DrainPendingWritesUpTo(TimeMasterT cutoff);

  // Replay dispatcher — applies one pending write's semantics. Writes that
  // only touch the shadow (open-bus ports) are no-ops here because the shadow
  // was updated at enqueue time. `cycle` is the write's original bus cycle,
  // used to timestamp emitted EmuEvents at the architecturally correct time.
  void ReplayWrite(uint16_t offset, uint8_t data, TimeMasterT cycle);

  // Frame-end signal handler — re-schedules itself for the next frame boundary.
  void OnFrameEndSignal(TimeMasterT master_time);

  // VBlank-NMI boundary signal handler. Serves as a scheduler sync fence at
  // the master cycle V transitions onto the VBlank entry line (falling edge
  // of the /NMI pin). Reschedules itself one frame out. Does not manipulate
  // time — MachineSync has already advanced every device to `master_time`
  // before firing. The rising edge of /NMI (V leaving the entry line) is NOT
  // fenced: the CPU's NMI flip-flop was already latched by the falling edge,
  // and $4210's line-end latch clear is observed lazily through
  // read-triggered PPU catch-up.
  void OnVblankNmiBoundarySignal(TimeMasterT master_time);

  // Dot-loop helpers.
  void AdvanceHv();
  void EmitPixel(uint32_t h, uint32_t v);
  // End-of-frame: swap the double buffers, fire the frontend callback with a
  // view over the (now-front) completed frame, and toggle `field_` for the
  // next frame's short-line selection.
  void OnEndOfFrame();

  // Linear framebuffer index for dot (h, v). Matches EmitPixel's own indexing.
  [[nodiscard]] static constexpr uint32_t FramebufferIndexFor(uint32_t h, uint32_t v) {
    return v * sppu::regs::kFrameBufferWidth + h;
  }

  // VRAM helpers. Address translation maps the raw vmadd_ to a rotated layout
  // per VMAIN bits 3:2; prefetch caches the translated word; increment fires
  // on the port matching VMAIN bit 7.
  [[nodiscard]] uint16_t TranslateVramAddress(uint16_t raw) const;
  [[nodiscard]] uint16_t VmainIncrementStep() const;
  void PrefetchVram();
  void MaybeIncrementVmaddOnPort(bool is_high_port);
  // RDVRAML/H side: same gate as MaybeIncrementVmaddOnPort, but the read port
  // first refreshes the prefetch latch with the word at the *current* vmadd_,
  // then advances vmadd_.
  void MaybeAdvanceVramReadOnPort(bool is_high_port);

  // Decoded BGxHOFS / BGxVOFS write through the shared "BG_old" latch.
  void WriteBgScroll(uint8_t bg, uint8_t data, bool is_hofs);
  // Decoded BG12NBA / BG34NBA character-base nibble pair (low → bg+0, high → bg+1).
  void WriteBgCharBase(uint8_t bg_pair_base, uint8_t data);
  // Read-twice flipflop body shared by OPHCT and OPVCT.
  [[nodiscard]] MmioReadResult ReadOpct(uint16_t counter, bool& read_high);

  // OAM helpers. Handles the byte-address wrap for the 32-byte high table
  // mirroring above $220; writes and reads all go through these.
  void WriteOamByte(uint16_t byte_addr, uint8_t data);
  [[nodiscard]] uint8_t ReadOamByte(uint16_t byte_addr) const;

  // Read a 16-bit word from VRAM (low byte at 2*word_addr, high at +1).
  // Word addresses wrap modulo 32K (mask &0x7FFF on the word index).
  [[nodiscard]] uint16_t ReadVramWord(uint16_t word_addr) const;

  // BG pixel fetch result. `cgram_index` is the BG-local palette index
  // ((palette_group << bpp) | color_index); the caller adds any per-mode
  // CGRAM region offset. Caller must have drained the lazy-replay log to
  // the dot's start cycle before calling FetchBgPixel.
  struct BgPixel {
    uint8_t cgram_index;
    bool transparent;
    bool priority;
  };
  [[nodiscard]] BgPixel FetchBgPixel(uint8_t bg, uint8_t bpp, uint32_t screen_x, uint32_t screen_y) const;

  // One BG's cached 8-pixel column. The 4 plane bytes are pre-selected for
  // the current row of the current (sub-)tile; pixel extraction only reads
  // the bit at column position within these bytes. Cleared at scanline
  // boundaries; invalidated mid-line via `bg_row_dirty_[bg]` when a register
  // write changes the data the cache holds (VRAM write, scroll, tilemap base,
  // tile size mode, etc.).
  struct BgRowCache {
    int32_t key = -1;  // (eff_x >> 3) when valid; -1 forces refetch
    uint8_t plane0 = 0;
    uint8_t plane1 = 0;
    uint8_t plane2 = 0;
    uint8_t plane3 = 0;
    uint8_t palette_group = 0;
    bool priority = false;
    bool hflip = false;
    bool is_2bpp = false;  // remembered so extraction skips planes 2/3 in 2bpp
  };

  // OBJ pixel at screen-space (x, y). Iterates the per-scanline OAM evaluation
  // list — built lazily by EvaluateObjLine the first time a new scanline asks
  // for an OBJ pixel — so the 128-sprite walk happens once per line rather
  // than once per dot. Lowest OAM index with non-transparent color wins.
  // `cgram_index` is absolute (already in the OBJ palette region $80-$FF).
  // The hardware 32-OBJ-per-line cap IS enforced (kObjLineCap).
  struct ObjPixel {
    uint8_t cgram_index;
    bool transparent;
    uint8_t priority;
  };
  [[nodiscard]] ObjPixel FetchObjPixel(uint32_t screen_x, uint32_t screen_y) const;

  // One sprite that overlaps the line cached in `obj_line_list_`. Stores the
  // per-line constants so per-pixel evaluation just iterates X-ranges + fetches
  // plane bytes. y_internal is `(screen_y - y_raw) & 0xFF` clipped to height —
  // already known to fall inside the sprite's vertical extent.
  struct ObjLineEntry {
    int16_t x;  // signed 9-bit X origin (sign-extended into int16_t)
    uint8_t y_internal;
    uint8_t width;
    uint8_t height;
    uint16_t base_tile;  // 9-bit tile number (bit 8 = region select)
    uint8_t attr;        // OAM byte 3: hflip/vflip/palette/priority
  };
  static constexpr std::size_t kObjLineCap = 32;
  // (Re)build `obj_line_list_` for the given visible scanline. Called lazily
  // by FetchObjPixel when obj_line_v_ doesn't match the current line.
  void EvaluateObjLine(uint32_t screen_y) const;

  // Output of a single-screen (main or sub) pixel resolution. `layer_id` runs
  // 0..3 = BG1..BG4, 4 = OBJ, 5 = backdrop (no opaque layer rendered).
  // `obj_palette_high` is meaningful only when layer_id == 4 — set when the
  // winning OBJ uses palette group 4..7 (the math-eligible OBJ palettes).
  struct ResolvedPixel {
    uint16_t bgr;
    uint8_t layer_id;
    bool obj_palette_high;
  };
  // Walk the active priority ladder for the current BGMODE/BG3-priority and
  // return the first opaque pixel whose layer is in `layer_mask`. The
  // pre-computed OBJ pixel is passed in so main + sub resolution share one
  // FetchObjPixel call per dot. Returns {backdrop colour, layer_id=5} when
  // nothing opaque renders.
  [[nodiscard]] ResolvedPixel ResolveScreenPixel(uint8_t layer_mask, uint32_t screen_x, uint32_t screen_y,
                                                 const ObjPixel& obj_px) const;

  // Apply $2131 CGADSUB math to the resolved main pixel. Sub source is the
  // sub-screen resolution when CGWSEL.1 is set and a sub-screen layer renders
  // here; otherwise the COLDATA fixed colour fills in. Returns BGR555.
  [[nodiscard]] uint16_t ApplyColorMath(uint16_t main_bgr, uint8_t main_layer, bool main_obj_high, uint32_t screen_x,
                                        uint32_t screen_y, const ObjPixel& obj_px) const;

  // --- Register shadow + decoded fields ---
  std::array<uint8_t, sppu::regs::kShadowSize> shadow_{};

  // Decoded INIDISP ($2100). Power-on: forced blank set, brightness 0.
  bool forced_blank_ = true;
  uint8_t brightness_ = 0;

  // Decoded BGMODE ($2105) + BGxSC / BGxNBA / BGxOFS / TM. Indices run
  // BG1=0..BG4=3; only BG1..BG3 participate in Mode 1.
  uint8_t bg_mode_ = 0;
  bool bg3_priority_ = false;
  std::array<bool, sppu::regs::kBgCount> bg_tile_16x16_{};
  std::array<uint16_t, sppu::regs::kBgCount> bg_tilemap_word_base_{};
  std::array<uint8_t, sppu::regs::kBgCount> bg_tilemap_layout_{};  // 0=32x32, 1=64x32, 2=32x64, 3=64x64
  std::array<uint16_t, sppu::regs::kBgCount> bg_char_word_base_{};
  // bg_hofs_ holds the raw 16-bit shift-in: the high byte preserves the
  // previous "Curr" so the next write's `(Reg_old>>8) & 7` feedback term
  // recovers all 3 of its low bits. Render path masks with kBgScrollMask.
  std::array<uint16_t, sppu::regs::kBgCount> bg_hofs_{};
  std::array<uint16_t, sppu::regs::kBgCount> bg_vofs_{};  // 10-bit (no feedback term, masked at write)
  // Shared "BG_old" latch byte. Per fullsnes: BGxHOFS = (Curr<<8) | (Prev&~7) |
  // ((BGxHOFS_old>>8)&7); BGxVOFS = (Curr<<8) | Prev. Prev = Curr after either.
  uint8_t bg_scroll_prev_ = 0;
  uint8_t main_screen_layers_ = 0;  // TM ($212C)
  uint8_t sub_screen_layers_ = 0;   // TS ($212D)

  // Decoded color math state.
  //   cgwsel_ / cgadsub_  — raw shadow of $2130 / $2131.
  //   coldata_r/g/b_      — accumulating per-channel intensity latches behind
  //                          $2132 (5-bit values). Each $2132 write updates
  //                          whichever channels it selects via bits 5/6/7;
  //                          unselected channels persist.
  uint8_t cgwsel_ = 0;
  uint8_t cgadsub_ = 0;
  uint8_t coldata_r_ = 0;
  uint8_t coldata_g_ = 0;
  uint8_t coldata_b_ = 0;

  // Decoded OBSEL ($2101). `obj_size_select_` chooses one of eight (small,
  // large) size pairs per fullsnes. `obj_region0_word_` / `obj_region1_word_`
  // are the precomputed tile-region word bases — region 0 from name_base
  // (8K-word steps), region 1 = region0 + 0x1000 + (name_select * 0x1000).
  uint8_t obj_size_select_ = 0;
  uint16_t obj_region0_word_ = 0;
  uint16_t obj_region1_word_ = 0;

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
  // update both the live pointer and the reload latch (copied back at
  // start-of-frame / during forced blank).
  uint16_t oam_byte_addr_ = 0;
  uint16_t oam_byte_addr_reload_ = 0;
  bool oam_priority_rotation_ = false;
  uint8_t oam_write_latch_ = 0;  // Low-byte latch for write-twice in low OAM.

  // Dot/scanline position + field toggle.
  uint32_t h_ = 0;
  uint32_t v_ = 0;
  bool field_ = false;

  // VBlank NMI latch. Armed inside AdvanceHv when V steps onto the VBlank
  // start line (225 normally, 240 under overscan); cleared on RDNMI ($4210)
  // read or when V wraps back to 0 at frame start.
  bool vblank_nmi_flag_ = false;

  // OPHCT/OPVCT latched H/V counters (9-bit) and per-register read-twice
  // flipflops. The latch fires on $2137 (SLHV) dummy-read, on a WRIO ($4201)
  // bit-7 1→0 transition, or on a lightgun pulse. We don't model WRIO yet —
  // its reset value FFh has bit 7 set, which is the gating condition fullsnes
  // calls out, so SLHV reads unconditionally trigger today. Both flipflops
  // reset on a $213F (STAT78) read; the latch flag (STAT78.bit6) also clears
  // on that read.
  uint16_t ophct_ = 0;
  uint16_t opvct_ = 0;
  bool ophct_read_high_ = false;
  bool opvct_read_high_ = false;
  bool hv_latch_flag_ = false;
  // Cycles already consumed toward the current dot from prior CatchUpTo calls.
  // Range: [0, DotCost(h_, v_, field_)). Lets one dot span multiple calls
  // when target lands mid-dot — no overshoot permitted.
  TimeMasterDeltaT partial_dot_cycles_ = 0;

  // --- Per-scanline OAM evaluation cache ---
  // `obj_line_v_` is the screen_y the cache was built for (or -1 to force a
  // rebuild). `obj_line_count_` is how many entries are populated, capped at
  // kObjLineCap. Mutable so FetchObjPixel can stay const while lazily
  // rebuilding — the eval result is pure derived state.
  mutable std::array<ObjLineEntry, kObjLineCap> obj_line_list_{};
  mutable uint8_t obj_line_count_ = 0;
  mutable int32_t obj_line_v_ = -1;

  // --- Per-BG row cache ---
  // Each FetchBgPixel call serves 8 consecutive pixels from a single decoded
  // tilemap entry. The cache reuses that decode while pixels stay inside the
  // same 8-pixel column; bg_row_dirty_ flags register writes that change the
  // underlying bytes (VRAM, scroll, BGnSC/NBA, BGMODE) and force the next
  // fetch to refresh. Mutable for the same reason as the OAM cache.
  mutable std::array<BgRowCache, sppu::regs::kBgCount> bg_row_cache_{};
  mutable std::array<bool, sppu::regs::kBgCount> bg_row_dirty_{};

  // --- Backing storage ---
  // Heap-allocated via unique_ptr<array> to keep the parent SNES object small
  // on the stack (64K VRAM + two 213KB framebuffers would otherwise blow it).
  std::unique_ptr<std::array<uint8_t, sppu::regs::kVramSize>> vram_;
  std::unique_ptr<std::array<uint8_t, sppu::regs::kOamSize>> oam_;
  std::unique_ptr<std::array<uint16_t, sppu::regs::kCgramWords>> cgram_;

  std::unique_ptr<std::array<uint16_t, sppu::regs::kFrameBufferPixels>> front_buffer_;
  std::unique_ptr<std::array<uint16_t, sppu::regs::kFrameBufferPixels>> back_buffer_;
  // One bit per pixel; set by CatchUpTo when the dot is emitted. Cleared on
  // frame wrap. Lets the PPU panel overlay distinguish "drawn this frame" from
  // "stale from the previous frame."
  std::unique_ptr<std::array<uint8_t, (sppu::regs::kFrameBufferPixels + 7U) / 8U>> drawn_mask_;

  // --- Pending-write log ---
  // Fixed-size to avoid allocation on the hot write path. DMA bursts and
  // HDMA can enqueue thousands of entries per frame; 16K covers the worst
  // case. Overflow triggers an internal flush via EnqueueWrite.
  static constexpr std::size_t kPendingWriteLogSize = 16384;
  std::unique_ptr<std::array<PpuPokeLogEntry, kPendingWriteLogSize>> pending_writes_;
  uint32_t pending_writes_count_ = 0;
  // Position within pending_writes_ of the next entry to replay. Writes with
  // a cycle above the current drain cutoff sit here until a later Tick picks
  // them up. Collapses to 0 once the tail is reached.
  uint32_t pending_writes_cursor_ = 0;
};

}  // namespace pupsnes
