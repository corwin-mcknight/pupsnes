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
inline constexpr uint8_t kInidispForcedBlankMask = 0x80;     // bit 7
inline constexpr uint8_t kInidispBrightnessMask = 0x0F;      // bits 3:0 (0=black, 15=full)

// OAM ports.
inline constexpr uint16_t kOamAddL = 0x2102;
inline constexpr uint16_t kOamAddH = 0x2103;
inline constexpr uint16_t kOamData = 0x2104;
inline constexpr uint8_t kOamAddHPriorityRotateMask = 0x80;  // bit 7 of $2103

// VRAM ports.
inline constexpr uint16_t kVmain = 0x2115;
inline constexpr uint8_t kVmainStepMask = 0x03;           // bits 1:0
inline constexpr uint8_t kVmainTranslateMask = 0x0C;      // bits 3:2
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

// Read ports.
inline constexpr uint16_t kRdOam = 0x2138;
inline constexpr uint16_t kRdVramL = 0x2139;
inline constexpr uint16_t kRdVramH = 0x213A;
inline constexpr uint16_t kRdCgram = 0x213B;

// $213E STAT77 — sprite overflow / time-over / version.
inline constexpr uint16_t kStat77 = 0x213E;
inline constexpr uint8_t kStat77VersionMask = 0x0F;  // bits 3:0

// $213F STAT78 — field / PAL / version.
inline constexpr uint16_t kStat78 = 0x213F;
inline constexpr uint8_t kStat78VersionMask = 0x0F;  // bits 3:0
inline constexpr uint8_t kStat78FieldMask = 0x80;    // bit 7
inline constexpr uint8_t kStat78PalMask = 0x10;      // bit 4 (0=NTSC, 1=PAL)

// Backing-store sizes.
inline constexpr std::size_t kVramSize = 64 * 1024;   // 64 KiB, word-addressed as 32K × 16
inline constexpr std::size_t kOamSize = 544;          // 512 B primary + 32 B high table
inline constexpr std::size_t kCgramWords = 256;       // 256 × 15-bit BGR words

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
inline constexpr uint32_t kVisibleHStart = 22;   // first emitted dot
inline constexpr uint32_t kVisibleHEnd = 278;    // exclusive (256-dot window)
inline constexpr uint32_t kVisibleVStartNtsc = 1;
inline constexpr uint32_t kVisibleVEnd224 = 225;  // exclusive (224-line mode)
inline constexpr uint32_t kVisibleVEnd239 = 240;  // exclusive (239-line overscan)
inline constexpr uint32_t kLogicalWidth = 256;

}  // namespace pupsnes::sppu::regs
