#pragma once

#include <cstddef>
#include <cstdint>

// 65C816-visible PPU (SPPU) register addresses and bit masks.
//
// All addresses live in the CPU's B-bus window at $2100-$213F, which is
// mirrored into banks $00-$3F and $80-$BF via `MapSystemBus`. Addresses are
// 24-bit SNES bus addresses; the PPU's ReadRegister / WriteRegister receives
// the post-mapping device offset where offset & 0x00FF is the B-bus index.
//
// Reference: fullsnes, Bruce Clark 65C816 docs (`docs/external/fullsnes.html`).
// Scope matches the PPU scaffold plan: v1 implements INIDISP, OAM/VRAM/CGRAM
// port machinery, SETINI overscan, and the PPU status registers. Everything
// else in $2100-$213F is shadow + open-bus for now.

namespace pupsnes::sppu::regs {

// Base of the B-bus PPU window. Offsets below subtract this to index the
// 64-byte shadow array.
inline constexpr uint16_t kBase = 0x2100;
inline constexpr uint16_t kEnd = 0x2140;  // exclusive — end of PPU-owned range
inline constexpr std::size_t kShadowSize = 0x40;

// $2100 INIDISP — Display control 1 (write-only).
inline constexpr uint16_t kInidisp = 0x2100;
inline constexpr uint8_t kInidispForcedBlankMask = 0x80;  // bit 7
inline constexpr uint8_t kInidispBrightnessMask = 0x0F;   // bits 3:0 (0=black, 15=full)

// $2105 BGMODE — BG mode + per-BG tile size + Mode-1 BG3 priority.
//   bits 2:0 = BG mode (0..7)
//   bit  3   = Mode-1 BG3 priority bit (1 → BG3 prio-1 tiles render above BG1)
//   bit  4   = BG1 tile size (0=8x8, 1=16x16)
//   bit  5   = BG2 tile size
//   bit  6   = BG3 tile size
//   bit  7   = BG4 tile size
inline constexpr uint16_t kBgmode = 0x2105;
inline constexpr uint8_t kBgmodeModeMask = 0x07;
inline constexpr uint8_t kBgmodeBg3PriorityMask = 0x08;
inline constexpr uint8_t kBgmodeBg1TileSizeMask = 0x10;
inline constexpr uint8_t kBgmodeBg2TileSizeMask = 0x20;
inline constexpr uint8_t kBgmodeBg3TileSizeMask = 0x40;
inline constexpr uint8_t kBgmodeBg4TileSizeMask = 0x80;

// $2107..$210A BGxSC — tilemap base address + multi-screen layout.
//   bits 7:2 = SC base address (in 1K-word / 2K-byte steps).
//   bits 1:0 = SC size: 0=32x32, 1=64x32, 2=32x64, 3=64x64.
inline constexpr uint16_t kBg1Sc = 0x2107;
inline constexpr uint16_t kBg2Sc = 0x2108;
inline constexpr uint16_t kBg3Sc = 0x2109;
inline constexpr uint16_t kBg4Sc = 0x210A;
inline constexpr uint8_t kBgScLayoutMask = 0x03;
inline constexpr uint8_t kBgScBaseMask = 0xFC;
// (data & kBgScBaseMask) << 8 yields a 16-bit word base in 0x400-word steps.

// $210B/$210C BG12NBA / BG34NBA — character (tile graphics) base addresses.
//   $210B low nibble = BG1, high nibble = BG2.
//   $210C low nibble = BG3, high nibble = BG4.
//   nibble * 0x1000 = word base in VRAM (4K-word / 8K-byte steps).
inline constexpr uint16_t kBg12Nba = 0x210B;
inline constexpr uint16_t kBg34Nba = 0x210C;

// $210D..$2114 BGxHOFS / BGxVOFS — BG scroll, 10-bit, write-twice via shared
// "BG_old" latch. $210D / $210E also drive M7HOFS/M7VOFS through a separate
// M7_old latch (not modeled here; M7 is out of scope for Mode 1).
inline constexpr uint16_t kBg1Hofs = 0x210D;
inline constexpr uint16_t kBg1Vofs = 0x210E;
inline constexpr uint16_t kBg2Hofs = 0x210F;
inline constexpr uint16_t kBg2Vofs = 0x2110;
inline constexpr uint16_t kBg3Hofs = 0x2111;
inline constexpr uint16_t kBg3Vofs = 0x2112;
inline constexpr uint16_t kBg4Hofs = 0x2113;
inline constexpr uint16_t kBg4Vofs = 0x2114;
inline constexpr uint16_t kBgScrollMask = 0x03FF;  // 10-bit field

// BG-map tilemap entry word (read from VRAM at the tilemap address):
//   bits  9:0  = character index (tile number within the BG's char region)
//   bits 12:10 = palette group
//   bit    13  = priority bit (per-tile, drives 2-level priority composition)
//   bit    14  = horizontal flip
//   bit    15  = vertical flip
inline constexpr uint16_t kBgMapEntryCharMask = 0x03FF;
inline constexpr uint16_t kBgMapEntryPaletteShift = 10;
inline constexpr uint16_t kBgMapEntryPaletteMask = 0x07;  // after shift
inline constexpr uint16_t kBgMapEntryPriorityMask = 0x2000;
inline constexpr uint16_t kBgMapEntryHflipMask = 0x4000;
inline constexpr uint16_t kBgMapEntryVflipMask = 0x8000;

// OAM low-table byte 3 (attributes) bit layout:
//   bit    0   = tile number bit 8 (selects tile region 1 when set)
//   bits  3:1  = palette group (0..7)
//   bits  5:4  = priority (0..3)
//   bit    6   = horizontal flip
//   bit    7   = vertical flip
inline constexpr uint8_t kObjAttrTileHighMask = 0x01;
inline constexpr uint8_t kObjAttrPaletteShift = 1;
inline constexpr uint8_t kObjAttrPaletteMask = 0x07;  // after shift
inline constexpr uint8_t kObjAttrPriorityShift = 4;
inline constexpr uint8_t kObjAttrPriorityMask = 0x03;  // after shift
inline constexpr uint8_t kObjAttrHflipMask = 0x40;
inline constexpr uint8_t kObjAttrVflipMask = 0x80;

// OAM high-table location: starts at byte $200 within the 544-byte OAM; each
// byte holds 2-bit (X-high, size) pairs for four OBJs (4 OBJs × 2 bits = 1 byte).
inline constexpr uint16_t kOamHighTableBase = 0x200;

// $212C TM — main-screen layer enable mask.
// $212D TS — sub-screen layer enable mask. Same bit layout as TM; the layer
// resolves via the same priority ladder but only the bits set here count.
inline constexpr uint16_t kTm = 0x212C;
inline constexpr uint16_t kTs = 0x212D;
inline constexpr uint8_t kTmBg1Mask = 0x01;
inline constexpr uint8_t kTmBg2Mask = 0x02;
inline constexpr uint8_t kTmBg3Mask = 0x04;
inline constexpr uint8_t kTmBg4Mask = 0x08;
inline constexpr uint8_t kTmObjMask = 0x10;

// $2130 CGWSEL — Color math control A.
//   bit 0   = Direct Color mode (256-color BG). Not modeled (out of scope).
//   bit 1   = Sub-screen BG/OBJ Enable. 0=sub-screen is COLDATA only,
//             1=sub-screen also renders BG/OBJ from TS, falling back to
//             COLDATA where transparent.
//   bits 5:4 = Color Math Enable region (0=always, 1=math-window,
//             2=outside math-window, 3=never). Math windows are out of scope
//             in v1; bits 1/2 are treated as "always" with a TODO.
//   bits 7:6 = Force Main-Screen Black region (same encoding). Treated as
//             "never force" (mode 0) in v1.
inline constexpr uint16_t kCgwsel = 0x2130;
inline constexpr uint8_t kCgwselDirectColorMask = 0x01;
inline constexpr uint8_t kCgwselSubScreenEnableMask = 0x02;
inline constexpr uint8_t kCgwselMathEnableRegionMask = 0x30;
inline constexpr uint8_t kCgwselMathEnableRegionShift = 4;
inline constexpr uint8_t kCgwselForceMainBlackRegionMask = 0xC0;
inline constexpr uint8_t kCgwselForceMainBlackRegionShift = 6;

// $2131 CGADSUB — Color math control B.
//   bits 0..3 = per-BG enable (BG1, BG2, BG3, BG4)
//   bit  4    = OBJ enable; only OBJ palettes 4..7 participate
//   bit  5    = backdrop enable
//   bit  6    = half-color math (1=halve final result per channel)
//   bit  7    = 0=add main+sub, 1=subtract main-sub
inline constexpr uint16_t kCgadsub = 0x2131;
inline constexpr uint8_t kCgadsubBg1Mask = 0x01;
inline constexpr uint8_t kCgadsubBg2Mask = 0x02;
inline constexpr uint8_t kCgadsubBg3Mask = 0x04;
inline constexpr uint8_t kCgadsubBg4Mask = 0x08;
inline constexpr uint8_t kCgadsubObjMask = 0x10;
inline constexpr uint8_t kCgadsubBackdropMask = 0x20;
inline constexpr uint8_t kCgadsubHalfMask = 0x40;
inline constexpr uint8_t kCgadsubSubtractMask = 0x80;

// $2132 COLDATA — fixed-color latches. Write-only; each write specifies
// which channels to update (bits 5/6/7 = R/G/B respectively) and the
// 5-bit intensity (bits 0..4). Channels are independent latches; the live
// 15-bit BGR fixed colour is assembled from the three latches at math time.
inline constexpr uint16_t kColdata = 0x2132;
inline constexpr uint8_t kColdataIntensityMask = 0x1F;
inline constexpr uint8_t kColdataApplyRedMask = 0x20;
inline constexpr uint8_t kColdataApplyGreenMask = 0x40;
inline constexpr uint8_t kColdataApplyBlueMask = 0x80;

// $2101 OBSEL — OBJ size + sprite tile name base/select.
//   bits 7:5 = OBJ size pair (0..5 documented + 6,7 undocumented; see fullsnes)
//   bits 4:3 = Name Select — gap between OBJ tile name regions, in 4K-word steps
//   bits 2:0 = Name Base for OBJ tiles 0x000..0x0FF, in 8K-word (16K-byte) steps
inline constexpr uint16_t kObsel = 0x2101;
inline constexpr uint8_t kObselSizeMask = 0xE0;
inline constexpr uint8_t kObselSizeShift = 5;
inline constexpr uint8_t kObselNameSelectMask = 0x18;
inline constexpr uint8_t kObselNameSelectShift = 3;
inline constexpr uint8_t kObselNameBaseMask = 0x07;

// OAM ports.
inline constexpr uint16_t kOamAddL = 0x2102;
inline constexpr uint16_t kOamAddH = 0x2103;
inline constexpr uint16_t kOamData = 0x2104;
inline constexpr uint8_t kOamAddHPriorityRotateMask = 0x80;  // bit 7 of $2103

// VRAM ports.
inline constexpr uint16_t kVmain = 0x2115;
inline constexpr uint8_t kVmainStepMask = 0x03;       // bits 1:0
inline constexpr uint8_t kVmainTranslateMask = 0x0C;  // bits 3:2
inline constexpr uint8_t kVmainTranslateShift = 2;
inline constexpr uint8_t kVmainIncrementOnHighMask = 0x80;  // bit 7 (0=inc on $2118, 1=inc on $2119)
inline constexpr uint16_t kVmAddL = 0x2116;
inline constexpr uint16_t kVmAddH = 0x2117;
inline constexpr uint16_t kVmDataL = 0x2118;
inline constexpr uint16_t kVmDataH = 0x2119;

// CGRAM ports.
inline constexpr uint16_t kCgAdd = 0x2121;
inline constexpr uint16_t kCgData = 0x2122;

// $2133 SETINI — Screen mode select.
inline constexpr uint16_t kSetini = 0x2133;
inline constexpr uint8_t kSetiniOverscanMask = 0x04;  // bit 2 (0=224 lines, 1=239)

// $2137 SLHV — Software latch for H/V counter. Reading this port triggers
// the same latch the lightgun / WRIO 1→0 transition would, capturing the
// current H/V counters into OPHCT/OPVCT and setting STAT78.bit6.
inline constexpr uint16_t kSlhv = 0x2137;

// Read ports.
inline constexpr uint16_t kRdOam = 0x2138;
inline constexpr uint16_t kRdVramL = 0x2139;
inline constexpr uint16_t kRdVramH = 0x213A;
inline constexpr uint16_t kRdCgram = 0x213B;

// $213C OPHCT — Horizontal counter latch (read-twice, 9-bit value).
// $213D OPVCT — Vertical counter latch (read-twice, 9-bit value).
// 1st read: bits 7:0 of the latched counter. 2nd read: bit 8 (only bit 0
// driven; bits 7:1 are PPU2 open-bus). Per-register 1st/2nd flipflops both
// reset on a STAT78 ($213F) read.
inline constexpr uint16_t kOphct = 0x213C;
inline constexpr uint16_t kOpvct = 0x213D;
inline constexpr uint8_t kOpctHighDrivenMask = 0x01;  // only bit 0 driven on 2nd read

// $213E STAT77 — sprite overflow / time-over / version.
inline constexpr uint16_t kStat77 = 0x213E;
inline constexpr uint8_t kStat77VersionMask = 0x0F;  // bits 3:0

// $213F STAT78 — field / PAL / version.
inline constexpr uint16_t kStat78 = 0x213F;
inline constexpr uint8_t kStat78VersionMask = 0x0F;    // bits 3:0
inline constexpr uint8_t kStat78FieldMask = 0x80;      // bit 7
inline constexpr uint8_t kStat78PalMask = 0x10;        // bit 4 (0=NTSC, 1=PAL)
inline constexpr uint8_t kStat78LatchFlagMask = 0x40;  // bit 6 — H/V latched

// Number of background layers (BG1..BG4).
inline constexpr uint8_t kBgCount = 4;

// Backing-store sizes.
inline constexpr std::size_t kVramSize = 64 * 1024;  // 64 KiB, word-addressed as 32K × 16
inline constexpr std::size_t kOamSize = 544;         // 512 B primary + 32 B high table
inline constexpr std::size_t kCgramWords = 256;      // 256 × 15-bit BGR words

// Dot / scanline timing constants (NTSC).
//
// Fullsnes: most scanlines are 1364 master cycles with two 6-cycle dots at
// H=323 and H=327 (338 × 4 + 2 × 6 = 1364). Short scanline skips the two
// 6-cycle dots → 1360 mcyc. Long scanline (PAL F=1 V=311) adds an extra dot
// for 1368 mcyc.
inline constexpr uint32_t kNormalLineCycles = 1364;
inline constexpr uint32_t kShortLineCycles = 1360;
inline constexpr uint32_t kLongLineCycles = 1368;
// Dots per scanline under the dot-major loop. Fullsnes: 338 × 4 + 2 × 6 =
// 1364 mcyc per normal NTSC line, i.e. 340 dots with H=323/327 being 6-cyc.
// Long PAL scanlines add one 4-cyc dot at H=340; the H/V grid below is sized
// for that case so both modes share the same framebuffer shape.
inline constexpr uint32_t kDotsPerLine = 340;
inline constexpr uint32_t kLongLineDotCount = 341;
inline constexpr uint32_t kLinesPerFrameNtsc = 262;
inline constexpr uint32_t kLinesPerFramePalNormal = 312;
inline constexpr uint32_t kLinesPerFramePalLong = 313;
inline constexpr uint32_t kFrameBufferWidth = 341;
inline constexpr uint32_t kFrameBufferHeight = 313;
inline constexpr std::size_t kFrameBufferPixels =
    static_cast<std::size_t>(kFrameBufferWidth) * static_cast<std::size_t>(kFrameBufferHeight);

// Visible drawing region (dot / scanline indices).
inline constexpr uint32_t kVisibleHStart = 22;  // first emitted dot
inline constexpr uint32_t kVisibleHEnd = 278;   // exclusive (256-dot window)
inline constexpr uint32_t kVisibleVStartNtsc = 1;
inline constexpr uint32_t kVisibleVEnd224 = 225;  // exclusive (224-line mode)
inline constexpr uint32_t kVisibleVEnd239 = 240;  // exclusive (239-line overscan)
inline constexpr uint32_t kLogicalWidth = 256;

}  // namespace pupsnes::sppu::regs
