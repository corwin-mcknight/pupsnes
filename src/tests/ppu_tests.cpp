#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>

#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/hw/sppu/ppu_regs.h"
#include "pupsnes/hw/systembus.h"

// PPU register / port behavior tests. Every test goes through the SystemBus
// so it exercises the real lazy-replay path: writes enqueue without catch-up,
// reads force a catch-up Tick that drains the log before sampling state.
//
// SNES-internal bus address for a PPU register: upper 8 bits = 0, middle 8 =
// $21 (the B-bus page), low 8 = the register offset. That resolves to the PPU
// device via SystemBus::MapPage done inside SNES::SNES().

using namespace pupsnes;  // NOLINT(google-build-using-namespace)

namespace {

void BusWrite(SNES& snes, SnesAddrT address, uint8_t data, TimeMasterT now) {
  BusPlan plan = snes.system_bus->Plan(address, BusAccessType::kWrite, data);
  REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
  (void)snes.system_bus->Follow(plan, now, 0);
}

[[nodiscard]] BusFollowResult BusRead(SNES& snes, SnesAddrT address, TimeMasterT now) {
  BusPlan plan = snes.system_bus->Plan(address, BusAccessType::kRead);
  REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
  return snes.system_bus->Follow(plan, now, 0);
}

}  // namespace

TEST_CASE("PPU Reset leaves INIDISP in forced-blank with brightness 0", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  REQUIRE(ppu.IsForcedBlank());
  REQUIRE(ppu.GetBrightness() == 0);
  REQUIRE(ppu.IsOverscan() == false);
  REQUIRE(ppu.GetPendingWriteCount() == 0);
}

TEST_CASE("INIDISP write queues, decoded only after a read catches up", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  // Disable forced blank, brightness = 0xF.
  BusWrite(snes, sppu::regs::kInidisp, 0x0F, /*now=*/100);
  REQUIRE(ppu.GetPendingWriteCount() == 1);
  // Decoded fields still carry the reset defaults — writes don't force
  // catch-up under the lazy-replay rule.
  REQUIRE(ppu.IsForcedBlank());
  REQUIRE(ppu.GetBrightness() == 0);

  // Any read of a PPU register triggers catch-up, which drains the log.
  (void)BusRead(snes, sppu::regs::kStat77, /*now=*/200);

  REQUIRE(ppu.IsForcedBlank() == false);
  REQUIRE(ppu.GetBrightness() == 0x0F);
  REQUIRE(ppu.GetPendingWriteCount() == 0);
}

TEST_CASE("SETINI bit 2 decodes overscan flag", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  BusWrite(snes, sppu::regs::kSetini, sppu::regs::kSetiniOverscanMask, /*now=*/10);
  (void)BusRead(snes, sppu::regs::kStat77, /*now=*/20);
  REQUIRE(ppu.IsOverscan());

  BusWrite(snes, sppu::regs::kSetini, 0x00, /*now=*/30);
  (void)BusRead(snes, sppu::regs::kStat77, /*now=*/40);
  REQUIRE(ppu.IsOverscan() == false);
}

TEST_CASE("Write-only PPU ports read as open-bus merged with last bus value", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  // Prime the bus latch: a fully-driven write to WRAM leaves 0xA5 on the bus.
  BusWrite(snes, 0x7E0000, 0xA5, /*now=*/10);

  // Read a write-only port (INIDISP is write-only). PPU returns {0, 0x00}
  // so the bus merge yields the last latch value, 0xA5.
  BusFollowResult result = BusRead(snes, sppu::regs::kInidisp, /*now=*/20);
  REQUIRE(result.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE(result.data == 0xA5);
}

TEST_CASE("CGDATA write-twice stores 15-bit word and auto-increments CGADD", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  // Target CGRAM word #5. Two CGDATA writes (low then high) commit one word.
  BusWrite(snes, sppu::regs::kCgAdd, 5, /*now=*/0);
  BusWrite(snes, sppu::regs::kCgData, 0x34, /*now=*/1);          // low byte
  BusWrite(snes, sppu::regs::kCgData, 0xFF, /*now=*/2);          // high byte; bit 7 dropped

  // Drain via any read.
  (void)BusRead(snes, sppu::regs::kStat77, /*now=*/3);

  // Low byte of [5] should be 0x34; high byte is 0xFF masked to 0x7F. Read
  // RDCGRAM twice: low, high. CGADD wrapped back to 5 after the write
  // committed one word, so CGADD is now 6. Rewind to 5 to read back.
  BusWrite(snes, sppu::regs::kCgAdd, 5, /*now=*/4);

  BusFollowResult low = BusRead(snes, sppu::regs::kRdCgram, /*now=*/5);
  BusFollowResult high = BusRead(snes, sppu::regs::kRdCgram, /*now=*/6);

  REQUIRE(low.data == 0x34);
  REQUIRE(high.data == 0x7F);  // 0xFF with bit 7 cleared
}

TEST_CASE("Writing CGADD resets the CGDATA write-twice latch", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  BusWrite(snes, sppu::regs::kCgAdd, 1, /*now=*/0);
  BusWrite(snes, sppu::regs::kCgData, 0x11, /*now=*/1);  // low byte latched
  // Interrupt the write-twice by setting CGADD again.
  BusWrite(snes, sppu::regs::kCgAdd, 2, /*now=*/2);
  // Next CGDATA write should start from the low byte again at CGADD=2.
  BusWrite(snes, sppu::regs::kCgData, 0x44, /*now=*/3);
  BusWrite(snes, sppu::regs::kCgData, 0x55, /*now=*/4);

  (void)BusRead(snes, sppu::regs::kStat77, /*now=*/5);  // drain

  // CGRAM[2] should be 0x5544; CGRAM[1] should be untouched (still 0).
  BusWrite(snes, sppu::regs::kCgAdd, 2, /*now=*/6);
  BusFollowResult low = BusRead(snes, sppu::regs::kRdCgram, /*now=*/7);
  BusFollowResult high = BusRead(snes, sppu::regs::kRdCgram, /*now=*/8);
  REQUIRE(low.data == 0x44);
  REQUIRE(high.data == 0x55);

  BusWrite(snes, sppu::regs::kCgAdd, 1, /*now=*/9);
  BusFollowResult orig_low = BusRead(snes, sppu::regs::kRdCgram, /*now=*/10);
  REQUIRE(orig_low.data == 0x00);
  (void)ppu;
}

TEST_CASE("VMDATA increments VMADD on the configured port only", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  // VMAIN: step=1, no translation, increment on $2118 (low) write.
  BusWrite(snes, sppu::regs::kVmain, 0x00, /*now=*/0);
  BusWrite(snes, sppu::regs::kVmAddL, 0x00, /*now=*/1);
  BusWrite(snes, sppu::regs::kVmAddH, 0x00, /*now=*/2);

  // Write low byte → VMADD should advance to 1.
  BusWrite(snes, sppu::regs::kVmDataL, 0xAA, /*now=*/3);
  // Write high byte of the new address → should NOT advance (because the
  // configured increment port is the low one). The high-byte write lands at
  // the high half of word 1.
  BusWrite(snes, sppu::regs::kVmDataH, 0xBB, /*now=*/4);

  // Now flip VMAIN to increment on $2119.
  BusWrite(snes, sppu::regs::kVmain, 0x80, /*now=*/5);
  BusWrite(snes, sppu::regs::kVmDataL, 0xCC, /*now=*/6);  // low: no increment
  BusWrite(snes, sppu::regs::kVmDataH, 0xDD, /*now=*/7);  // high: increments

  // Drain and verify by reading via the prefetch mechanism. Reset VMADD to
  // verify the writes took effect at the right byte offsets.
  BusWrite(snes, sppu::regs::kVmAddL, 0x00, /*now=*/8);
  BusWrite(snes, sppu::regs::kVmAddH, 0x00, /*now=*/9);
  // After the $2117 write, the PPU prefetches word 0. Read it via RDVRAML/H.
  BusFollowResult word0_lo = BusRead(snes, sppu::regs::kRdVramL, /*now=*/10);
  BusFollowResult word0_hi = BusRead(snes, sppu::regs::kRdVramH, /*now=*/11);

  REQUIRE(word0_lo.data == 0xAA);
  REQUIRE(word0_hi.data == 0x00);  // high byte of word 0 never written
  (void)ppu;
}

TEST_CASE("RDVRAM honors the prefetch-buffer quirk", "[unit][ppu]") {
  // Real SNES quirk: the prefetch buffer refills from `vram[VMADDR]` BEFORE
  // the post-read increment. The returned byte always trails by one step.
  //
  //   After VMADDR write: prefetch = vram[VMADDR]          (pre-loaded)
  //   RDVRAM*  → return prefetch; if port matches inc bit:
  //              prefetch = vram[VMADDR]   (refill at current VMADDR)
  //              VMADDR += step            (then increment)
  //
  // The concrete observable symptom: with step=1 starting at VMADDR=0 and
  // increment-on-high, you need TWO RDVRAMH reads before the first RDVRAML
  // read returns word 1's low byte.
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  // Seed: word 0 = 0xCAFE, word 1 = 0xBABE. Use increment-on-high to write.
  BusWrite(snes, sppu::regs::kVmain, 0x80, /*now=*/0);
  BusWrite(snes, sppu::regs::kVmAddL, 0x00, /*now=*/1);
  BusWrite(snes, sppu::regs::kVmAddH, 0x00, /*now=*/2);
  BusWrite(snes, sppu::regs::kVmDataL, 0xFE, /*now=*/3);
  BusWrite(snes, sppu::regs::kVmDataH, 0xCA, /*now=*/4);  // commits word 0; vmadd→1
  BusWrite(snes, sppu::regs::kVmDataL, 0xBE, /*now=*/5);
  BusWrite(snes, sppu::regs::kVmDataH, 0xBA, /*now=*/6);  // commits word 1; vmadd→2

  // Reset VMADDR to 0 — the $2117 write triggers a prefetch of word 0.
  BusWrite(snes, sppu::regs::kVmAddL, 0x00, /*now=*/7);
  BusWrite(snes, sppu::regs::kVmAddH, 0x00, /*now=*/8);

  // Three back-to-back (RDVRAML, RDVRAMH) pairs — the quirk means the first
  // two pairs both return word 0; only the third pair sees word 1.
  BusFollowResult r1_lo = BusRead(snes, sppu::regs::kRdVramL, /*now=*/9);
  BusFollowResult r1_hi = BusRead(snes, sppu::regs::kRdVramH, /*now=*/10);
  BusFollowResult r2_lo = BusRead(snes, sppu::regs::kRdVramL, /*now=*/11);
  BusFollowResult r2_hi = BusRead(snes, sppu::regs::kRdVramH, /*now=*/12);
  BusFollowResult r3_lo = BusRead(snes, sppu::regs::kRdVramL, /*now=*/13);
  BusFollowResult r3_hi = BusRead(snes, sppu::regs::kRdVramH, /*now=*/14);

  REQUIRE(r1_lo.data == 0xFE);
  REQUIRE(r1_hi.data == 0xCA);
  // Still word 0 — prefetch refilled from vmadd=0 on the previous RDVRAMH,
  // before the increment stepped it to 1.
  REQUIRE(r2_lo.data == 0xFE);
  REQUIRE(r2_hi.data == 0xCA);
  // Now word 1 surfaces. The previous RDVRAMH refilled from vmadd=1 before
  // stepping to 2.
  REQUIRE(r3_lo.data == 0xBE);
  REQUIRE(r3_hi.data == 0xBA);
  (void)ppu;
}

TEST_CASE("OAM low-range write is write-twice, high-range is byte-wise", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  // Point at OAM word 3 (byte address 6 in low OAM) and write two bytes.
  BusWrite(snes, sppu::regs::kOamAddL, 0x03, /*now=*/0);
  BusWrite(snes, sppu::regs::kOamAddH, 0x00, /*now=*/1);
  BusWrite(snes, sppu::regs::kOamData, 0x11, /*now=*/2);  // latches, addr 6 -> 7
  BusWrite(snes, sppu::regs::kOamData, 0x22, /*now=*/3);  // commits oam[6]=0x11, oam[7]=0x22

  // Point at OAM high-table word 0 (byte 0x200). Each OAMDATA write commits
  // a single byte, no latch.
  BusWrite(snes, sppu::regs::kOamAddL, 0x00, /*now=*/4);
  BusWrite(snes, sppu::regs::kOamAddH, 0x01, /*now=*/5);  // bit 0 of H = word bit 8
  BusWrite(snes, sppu::regs::kOamData, 0xAA, /*now=*/6);  // commits oam[0x200]=0xAA directly

  // Drain + read back via RDOAM.
  (void)BusRead(snes, sppu::regs::kStat77, /*now=*/7);

  BusWrite(snes, sppu::regs::kOamAddL, 0x03, /*now=*/8);
  BusWrite(snes, sppu::regs::kOamAddH, 0x00, /*now=*/9);
  BusFollowResult b6 = BusRead(snes, sppu::regs::kRdOam, /*now=*/10);
  BusFollowResult b7 = BusRead(snes, sppu::regs::kRdOam, /*now=*/11);
  REQUIRE(b6.data == 0x11);
  REQUIRE(b7.data == 0x22);

  BusWrite(snes, sppu::regs::kOamAddL, 0x00, /*now=*/12);
  BusWrite(snes, sppu::regs::kOamAddH, 0x01, /*now=*/13);
  BusFollowResult high0 = BusRead(snes, sppu::regs::kRdOam, /*now=*/14);
  REQUIRE(high0.data == 0xAA);
  (void)ppu;
}

TEST_CASE("STAT77 version field reads as PPU1 version 1 with open-bus upper bits", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  // Prime the latch with 0xF0 so we can tell driven vs floating bits apart.
  BusWrite(snes, 0x7E0000, 0xF0, /*now=*/0);
  BusFollowResult stat = BusRead(snes, sppu::regs::kStat77, /*now=*/1);
  // Bits 3:0 driven as version 1; bits 7:4 come from the bus latch (0xF).
  REQUIRE(stat.data == 0xF1);
  (void)ppu;
}

TEST_CASE("STAT78 surfaces version, PAL=0, field toggle, and open-bus bits", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  // Same latch priming trick: 0xFF means every open-bus bit surfaces as 1.
  BusWrite(snes, 0x7E0000, 0xFF, /*now=*/0);
  BusFollowResult stat = BusRead(snes, sppu::regs::kStat78, /*now=*/1);
  // Driven bits: version=0x03 (bits 3:0), PAL=0 (bit 4), field=0 on reset
  // (bit 7). Open-bus bits 5:6 come from the latch, so they're 1.
  // Expected: 0b 0 11 0 0011 = 0x63 combined with open-bus bits 6:5 set → 0x63.
  REQUIRE(stat.data == 0x63);
  (void)ppu;
}

TEST_CASE("DotCost: most dots are 4 mcyc; H=323 and H=327 cost 6 on normal lines", "[unit][ppu]") {
  // Fullsnes: normal scanline = 1364 mcyc. Every dot is 4 cycles except H=323
  // and H=327 which are 6. Short-line V=240 on the odd field collapses those
  // two back to 4-cycle dots (1360 mcyc total).
  REQUIRE(Ppu::DotCost(0, 0, false) == 4);
  REQUIRE(Ppu::DotCost(322, 0, false) == 4);
  REQUIRE(Ppu::DotCost(323, 0, false) == 6);
  REQUIRE(Ppu::DotCost(324, 0, false) == 4);
  REQUIRE(Ppu::DotCost(327, 0, false) == 6);
  REQUIRE(Ppu::DotCost(328, 0, false) == 4);
  REQUIRE(Ppu::DotCost(340, 0, false) == 4);

  REQUIRE(Ppu::DotCost(323, 240, true) == 4);
  REQUIRE(Ppu::DotCost(327, 240, true) == 4);
  REQUIRE(Ppu::DotCost(323, 240, false) == 6);
  REQUIRE(Ppu::DotCost(323, 100, true) == 6);

  REQUIRE(Ppu::LineCycles(0, false) == 1364);
  REQUIRE(Ppu::LineCycles(240, false) == 1364);
  REQUIRE(Ppu::LineCycles(240, true) == 1360);
  REQUIRE(Ppu::LineCycles(261, true) == 1364);
}

TEST_CASE("BrightnessScale: fullsnes formula (channel * (b+1)) >> 4", "[unit][ppu]") {
  // Brightness 15 passes through unchanged (factor = 16, shift 4 = identity).
  REQUIRE(Ppu::BrightnessScale(0x7FFF, 15) == 0x7FFF);
  REQUIRE(Ppu::BrightnessScale(0x001F, 15) == 0x001F);  // pure red full
  REQUIRE(Ppu::BrightnessScale(0x03E0, 15) == 0x03E0);  // pure green full
  REQUIRE(Ppu::BrightnessScale(0x7C00, 15) == 0x7C00);  // pure blue full

  // Brightness 0 collapses every channel to channel/16, rounded down.
  REQUIRE(Ppu::BrightnessScale(0x001F, 0) == 0x0001);
  REQUIRE(Ppu::BrightnessScale(0x0010, 0) == 0x0001);
  REQUIRE(Ppu::BrightnessScale(0x000F, 0) == 0x0000);

  // Half-brightness (7): factor = 8, shift = 4 → channel/2 truncated.
  REQUIRE(Ppu::BrightnessScale(0x001F, 7) == 0x000F);
  REQUIRE(Ppu::BrightnessScale(0x0014, 7) == 0x000A);
}

namespace {

// Count frame-ready callbacks and capture the most recent view so tests can
// inspect the landed frame.
struct FrameReadyProbe {
  int count = 0;
  FrameBufferView last_view{};
};

void WireFrameProbe(SNES& snes, FrameReadyProbe& probe) {
  snes.SetFrameReadyCallback([&](const FrameBufferView& view) {
    ++probe.count;
    probe.last_view = view;
  });
}

}  // namespace

TEST_CASE("PPU runs a full NTSC frame, fires onFrameReady, emits backdrop pixels", "[unit][ppu][integration]") {
  SNES snes;
  FrameReadyProbe probe;
  WireFrameProbe(snes, probe);
  snes.Reset();

  Ppu& ppu = snes.GetPpu();

  // Seed CGRAM[0] with pure red (BGR555: R=31, G=0, B=0) and disable forced
  // blank with full brightness. Writes arrive before any dot emits.
  BusWrite(snes, sppu::regs::kCgAdd, 0, /*now=*/0);
  BusWrite(snes, sppu::regs::kCgData, 0x1F, /*now=*/1);  // low byte: R=0x1F
  BusWrite(snes, sppu::regs::kCgData, 0x00, /*now=*/2);  // high byte: G=B=0
  BusWrite(snes, sppu::regs::kInidisp, 0x0F, /*now=*/3);  // brightness=15, forced_blank=0

  // One full NTSC non-interlace frame = 262 * 1364 = 357368 cycles.
  constexpr TimeMasterT kFrameEnd = 262U * 1364U;
  ppu.CatchUpTo(kFrameEnd);

  REQUIRE(probe.count == 1);
  REQUIRE(probe.last_view.width == 256);
  REQUIRE(probe.last_view.height == 224);
  REQUIRE(probe.last_view.stride == sppu::regs::kFrameBufferWidth);

  // Visible (V=100, H=100) after brightness pass-through = raw cgram[0].
  const uint16_t pixel = ppu.GetFrontBuffer()[100U * sppu::regs::kFrameBufferWidth + 100U];
  REQUIRE(pixel == 0x001F);

  // Out-of-visible pixels stay black: H=5 is left of the visible window.
  const uint16_t blanked = ppu.GetFrontBuffer()[100U * sppu::regs::kFrameBufferWidth + 5U];
  REQUIRE(blanked == 0x0000);
}

TEST_CASE("Frame cadence: normal frames = 357368 mcyc, short-line frames = 357364", "[unit][ppu][integration]") {
  SNES snes;
  FrameReadyProbe probe;
  WireFrameProbe(snes, probe);
  snes.Reset();

  Ppu& ppu = snes.GetPpu();

  // Frame 1 starts with field=false → V=240 is normal (1364). Total 357368.
  constexpr TimeMasterT kFrame1End = 262U * 1364U;
  ppu.CatchUpTo(kFrame1End);
  REQUIRE(probe.count == 1);
  REQUIRE(ppu.GetField() == true);  // toggled at end-of-frame

  // Frame 2 has field=true → V=240 is the short 1360-cycle line. Total = 357364.
  constexpr TimeMasterT kFrame2End = kFrame1End + 261U * 1364U + 1360U;
  ppu.CatchUpTo(kFrame2End);
  REQUIRE(probe.count == 2);
  REQUIRE(ppu.GetField() == false);  // toggled back
  REQUIRE(ppu.GetDotH() == 0);
  REQUIRE(ppu.GetDotV() == 0);
}

TEST_CASE("PPU frame output is deterministic across runs", "[unit][ppu][integration]") {
  auto run_one_frame = [](uint16_t* dest) {
    SNES snes;
    snes.Reset();
    Ppu& ppu = snes.GetPpu();
    // A small CGRAM palette + brightness ramp keeps the determinism check
    // non-trivial without needing any CPU execution.
    BusWrite(snes, sppu::regs::kCgAdd, 0, /*now=*/0);
    BusWrite(snes, sppu::regs::kCgData, 0xAA, /*now=*/1);
    BusWrite(snes, sppu::regs::kCgData, 0x55, /*now=*/2);
    BusWrite(snes, sppu::regs::kInidisp, 0x07, /*now=*/3);

    constexpr TimeMasterT kFrameEnd = 262U * 1364U;
    ppu.CatchUpTo(kFrameEnd);

    std::memcpy(dest, ppu.GetFrontBuffer(),
                sppu::regs::kFrameBufferPixels * sizeof(uint16_t));
  };

  std::array<uint16_t, sppu::regs::kFrameBufferPixels> a{};
  std::array<uint16_t, sppu::regs::kFrameBufferPixels> b{};
  run_one_frame(a.data());
  run_one_frame(b.data());
  REQUIRE(std::memcmp(a.data(), b.data(), a.size() * sizeof(uint16_t)) == 0);
}

TEST_CASE("Forced blank keeps the backbuffer black inside the visible window", "[unit][ppu][integration]") {
  SNES snes;
  FrameReadyProbe probe;
  WireFrameProbe(snes, probe);
  snes.Reset();
  Ppu& ppu = snes.GetPpu();

  // Configure a red backdrop but leave forced blank set (reset default).
  BusWrite(snes, sppu::regs::kCgAdd, 0, /*now=*/0);
  BusWrite(snes, sppu::regs::kCgData, 0x1F, /*now=*/1);
  BusWrite(snes, sppu::regs::kCgData, 0x00, /*now=*/2);

  constexpr TimeMasterT kFrameEnd = 262U * 1364U;
  ppu.CatchUpTo(kFrameEnd);

  REQUIRE(probe.count == 1);
  REQUIRE(ppu.IsForcedBlank());
  // Even the visible center is 0 under forced blank.
  const uint16_t center = ppu.GetFrontBuffer()[100U * sppu::regs::kFrameBufferWidth + 100U];
  REQUIRE(center == 0x0000);
}


TEST_CASE("PPU drawn mask tracks pixels emitted since last frame start", "[unit][ppu]") {
  // After Reset, drawn_mask is all-zero. Each call to CatchUpTo advances the
  // dot loop and sets a bit per emitted pixel. Bits are stored LSB-first
  // within each byte: pixel index N → byte N/8, bit N%8.
  SNES snes;
  snes.Reset();
  Ppu& ppu = snes.GetPpu();
  const uint8_t* mask = ppu.GetDrawnMask();

  // Mask starts zero — no pixels have been emitted yet.
  REQUIRE(mask[0] == 0);

  // Each dot costs 4 mcyc (DotCost(0,0,false)==4). Advancing by 12 mcyc
  // completes exactly 3 dots at positions idx 0, 1, 2 → bits 0, 1, 2.
  ppu.CatchUpTo(ppu.GetTime() + 12);
  REQUIRE((mask[0] & 0b0000'0111) == 0b0000'0111);
}

TEST_CASE("PPU reschedules kFrameEnd after every frame boundary", "[unit][ppu]") {
  // Reset schedules the first kFrameEnd signal. FireEventsThrough must
  // trigger OnFrameEndSignal, which chains the next event. The second event's
  // master_time must be strictly greater than the first.
  SNES snes;
  snes.Reset();

  const TimeMasterT first = snes.GetScheduler().NextEventMasterTime();
  REQUIRE(first > 0);
  // One NTSC frame is at most 262 * 1364 = 357368 mcyc.
  REQUIRE(first <= 262U * 1364U);

  // Drive the PPU up to the frame boundary so OnFrameEndSignal sees a
  // consistent state, then fire the event.
  snes.GetPpu().CatchUpTo(first);
  snes.GetScheduler().FireEventsThrough(first);

  const TimeMasterT second = snes.GetScheduler().NextEventMasterTime();
  REQUIRE(second > first);
}

TEST_CASE("Shadow captures every write in $2100-$213F for debugger round-trip", "[unit][ppu]") {
  // Phase F coverage: the debugger panel reads Ppu::GetShadow to show the
  // last-written value for any PPU register, independent of whether the
  // register has a real decoded representation. Every $2100-$213F write
  // must land in the shadow byte at `reg - 0x2100`.
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  for (uint16_t reg = sppu::regs::kBase; reg < sppu::regs::kEnd; ++reg) {
    const uint8_t marker = static_cast<uint8_t>(0x40U + (reg - sppu::regs::kBase));
    BusWrite(snes, reg, marker, /*now=*/static_cast<TimeMasterT>(reg - sppu::regs::kBase));
  }

  // A read drains the log. Shadows are updated at enqueue time already, so
  // this mostly checks that replay doesn't stomp them.
  (void)BusRead(snes, sppu::regs::kStat77, /*now=*/0x1000);

  for (uint16_t reg = sppu::regs::kBase; reg < sppu::regs::kEnd; ++reg) {
    const uint8_t expected = static_cast<uint8_t>(0x40U + (reg - sppu::regs::kBase));
    INFO("reg=$" << std::hex << reg);
    REQUIRE(ppu.GetShadow(reg) == expected);
  }
}

TEST_CASE("VMAIN step size selects +1, +32, +128 VMADD advance", "[unit][ppu]") {
  // Exercise every step encoding (0b00=+1, 0b01=+32, 0b10=+128, 0b11=+128).
  // After two VMDATAL writes, VMADD has advanced by 2 × step. Seeding vram
  // at the advance target and reading back through mode 0 verifies the
  // second write landed at the expected byte address.
  auto verify_step = [](uint8_t vmain_low_bits, uint16_t expected_step) {
    SNES snes;
    Ppu& ppu = snes.GetPpu();
    ppu.Reset();

    // Step bits in [1:0], mode 0 in [3:2], inc on VMDATAL write (bit 7 clear).
    const uint8_t vmain = vmain_low_bits;
    BusWrite(snes, sppu::regs::kVmain, vmain, /*now=*/0);
    BusWrite(snes, sppu::regs::kVmAddL, 0x00, /*now=*/1);
    BusWrite(snes, sppu::regs::kVmAddH, 0x00, /*now=*/2);

    BusWrite(snes, sppu::regs::kVmDataL, 0xAA, /*now=*/3);
    BusWrite(snes, sppu::regs::kVmDataL, 0xBB, /*now=*/4);

    // Re-seed VMADD to `expected_step` (where the second write should have
    // landed). Prefetch fires on $2117, RDVRAML returns the low byte.
    BusWrite(snes, sppu::regs::kVmAddL, static_cast<uint8_t>(expected_step & 0xFFU), /*now=*/5);
    BusWrite(snes, sppu::regs::kVmAddH, static_cast<uint8_t>((expected_step >> 8U) & 0xFFU), /*now=*/6);
    const BusFollowResult second = BusRead(snes, sppu::regs::kRdVramL, /*now=*/7);
    REQUIRE(second.data == 0xBB);
    (void)ppu;
  };

  SECTION("step +1") { verify_step(0x00, 1); }
  SECTION("step +32") { verify_step(0x01, 32); }
  SECTION("step +128 (bits 10)") { verify_step(0x02, 128); }
  SECTION("step +128 (bits 11)") { verify_step(0x03, 128); }
}

TEST_CASE("VMAIN address translation modes rotate the low address bits", "[unit][ppu]") {
  // Fullsnes: modes 01/10/11 rotate the low 8/9/10 bits of the VRAM word
  // address. Writing at raw VMADD X with translation mode M lands at
  // translated(M, X). We verify by writing through a translated path and
  // re-reading via mode 0 at the expected translated address.
  auto verify_translate = [](uint8_t mode_bits, uint16_t raw_word_addr, uint16_t expected_translated_word_addr) {
    SNES snes;
    Ppu& ppu = snes.GetPpu();
    ppu.Reset();

    const uint8_t vmain = static_cast<uint8_t>(
        sppu::regs::kVmainIncrementOnHighMask | (mode_bits << sppu::regs::kVmainTranslateShift));
    BusWrite(snes, sppu::regs::kVmain, vmain, /*now=*/0);
    BusWrite(snes, sppu::regs::kVmAddL, static_cast<uint8_t>(raw_word_addr & 0xFFU), /*now=*/1);
    BusWrite(snes, sppu::regs::kVmAddH, static_cast<uint8_t>((raw_word_addr >> 8U) & 0xFFU), /*now=*/2);
    BusWrite(snes, sppu::regs::kVmDataL, 0xAA, /*now=*/3);
    BusWrite(snes, sppu::regs::kVmDataH, 0xBB, /*now=*/4);

    // Read back through mode 0 (no translation).
    BusWrite(snes, sppu::regs::kVmain, sppu::regs::kVmainIncrementOnHighMask, /*now=*/5);
    BusWrite(snes, sppu::regs::kVmAddL, static_cast<uint8_t>(expected_translated_word_addr & 0xFFU), /*now=*/6);
    BusWrite(snes, sppu::regs::kVmAddH, static_cast<uint8_t>((expected_translated_word_addr >> 8U) & 0xFFU),
             /*now=*/7);
    const BusFollowResult lo = BusRead(snes, sppu::regs::kRdVramL, /*now=*/8);
    const BusFollowResult hi = BusRead(snes, sppu::regs::kRdVramH, /*now=*/9);
    REQUIRE(lo.data == 0xAA);
    REQUIRE(hi.data == 0xBB);
    (void)ppu;
  };

  // Mode 01: aaaaaaaa YYYxxxxx → aaaaaaaa xxxxxYYY
  // raw 0x001F (YYY=000, xxxxx=11111) → 0x00F8 (11111 000)
  SECTION("mode 01 (8x32)") { verify_translate(1, 0x001F, 0x00F8); }

  // Mode 10: aaaaaaa YYYxxxxxx → aaaaaaa xxxxxxYYY
  // raw 0x003F (YYY=000, xxxxxx=111111) → 0x01F8 (111111 000)
  SECTION("mode 10 (8x64)") { verify_translate(2, 0x003F, 0x01F8); }

  // Mode 11: aaaaaa YYYxxxxxxx → aaaaaa xxxxxxxYYY
  // raw 0x007F (YYY=000, xxxxxxx=1111111) → 0x03F8 (1111111 000)
  SECTION("mode 11 (8x128)") { verify_translate(3, 0x007F, 0x03F8); }
}

TEST_CASE("DMA-style burst: hundreds of writes never tick the PPU", "[unit][ppu][integration]") {
  // Plan F3: a burst of writes to $2122 (or any PPU reg) must not trigger a
  // per-write catch-up. The PPU's local_time stays at 0 throughout; the log
  // grows monotonically; a single subsequent read drains everything.
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  constexpr uint32_t kBurst = 500;
  for (uint32_t i = 0; i < kBurst; ++i) {
    BusWrite(snes, sppu::regs::kCgData, static_cast<uint8_t>(i & 0xFFU), static_cast<TimeMasterT>(i));
    REQUIRE(ppu.GetTime() == 0);
    REQUIRE(ppu.GetPendingWriteCount() == i + 1U);
  }

  (void)BusRead(snes, sppu::regs::kStat77, /*now=*/kBurst + 100);
  REQUIRE(ppu.GetPendingWriteCount() == 0);
}

TEST_CASE("Pending-write log overflow flushes synchronously without losing writes", "[unit][ppu][integration]") {
  // Plan B4: the 16384-entry log is a soft limit; overflow forces an
  // internal flush rather than dropping writes. We prove that the final
  // write (the 16385th) is still observable after draining.
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  // Fill the log with shadow-only writes that ReplayWrite treats as no-ops
  // (BGMODE at $2105 is in range but not implemented). This keeps the
  // flush cheap while still exercising the overflow path.
  constexpr uint32_t kLogSize = 16384;
  for (uint32_t i = 0; i < kLogSize; ++i) {
    BusWrite(snes, 0x2105U, static_cast<uint8_t>(i & 0xFFU), static_cast<TimeMasterT>(i));
  }
  REQUIRE(ppu.GetPendingWriteCount() == kLogSize);

  // The next enqueue must trigger the internal flush. Use a real register
  // (INIDISP) with a recognisable marker so we can verify it survived.
  BusWrite(snes, sppu::regs::kInidisp, 0x0F, /*now=*/kLogSize);
  // After the flush, the queue holds only the most recent entry.
  REQUIRE(ppu.GetPendingWriteCount() == 1);

  (void)BusRead(snes, sppu::regs::kStat77, /*now=*/kLogSize + 1);
  REQUIRE(ppu.IsForcedBlank() == false);
  REQUIRE(ppu.GetBrightness() == 0x0F);
}

TEST_CASE("Read drains writes queued after the PPU last ticked", "[unit][ppu][integration]") {
  // Plan F3 / lazy-replay contract: any write with cycle <= bus_read_time
  // must be visible to the read, even if the dot loop's per-dot drain
  // stopped short of that cycle. ReadRegister calls DrainPendingWritesUpTo
  // to guarantee this.
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  // First catch-up leaves the PPU at T=500, mid-scanline. The dot loop
  // drains writes up to each dot's start time.
  (void)BusRead(snes, sppu::regs::kStat77, /*now=*/500);
  const TimeMasterT ppu_time = ppu.GetTime();
  REQUIRE(ppu_time >= 500);

  // Queue a write at cycle = ppu_time + 1 — a cycle the next Tick would
  // drain naturally, but we read at exactly that cycle and expect the
  // read's drain to surface the write.
  const TimeMasterT write_cycle = ppu_time + 1;
  BusWrite(snes, sppu::regs::kInidisp, 0x0F, write_cycle);

  const TimeMasterT read_cycle = write_cycle;
  (void)BusRead(snes, sppu::regs::kStat77, read_cycle);
  REQUIRE(ppu.IsForcedBlank() == false);
  REQUIRE(ppu.GetBrightness() == 0x0F);
}

TEST_CASE("Lazy replay — many writes incur a single catch-up", "[unit][ppu]") {
  // Regression: DMA-style bursts should not force one PPU Tick per write.
  // The pending-write log accumulates, and a single read drains all of them.
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  for (uint8_t i = 0; i < 64; ++i) {
    BusWrite(snes, sppu::regs::kInidisp, i, /*now=*/static_cast<TimeMasterT>(i));
  }
  REQUIRE(ppu.GetPendingWriteCount() == 64);
  REQUIRE(ppu.GetTime() == 0);  // No catch-up ticked the PPU yet.

  (void)BusRead(snes, sppu::regs::kStat77, /*now=*/200);

  REQUIRE(ppu.GetPendingWriteCount() == 0);
  REQUIRE(ppu.GetTime() == 200);
  // Last INIDISP write was 63 (0x3F) — forced_blank clear, brightness 0xF.
  REQUIRE(ppu.IsForcedBlank() == false);
  REQUIRE(ppu.GetBrightness() == 0x0F);
}

TEST_CASE("PPU QueryHvbStatus tracks VBlank and HBlank transitions", "[unit][ppu]") {
  SNES snes;
  snes.Reset();
  Ppu& ppu = snes.GetPpu();

  // At power-on both flags are clear: v=0, h=0.
  auto status = ppu.QueryHvbStatus(0);
  REQUIRE_FALSE(status.vblank);
  REQUIRE_FALSE(status.hblank);

  // Dot cost = 4 mcyc for H in [0, 322]. Catching up 274 dots puts h_ at 274
  // with v_ still 0 — HBlank set, VBlank clear.
  status = ppu.QueryHvbStatus(274U * 4U);
  REQUIRE_FALSE(status.vblank);
  REQUIRE(status.hblank);

  // Advance to the start of scanline 225 (first VBlank line, no overscan).
  // Every line up to V=239 is 1364 mcyc (no short-line work at field=false).
  constexpr TimeMasterT kStartOfV225 = 225U * 1364U;
  status = ppu.QueryHvbStatus(kStartOfV225);
  REQUIRE(status.vblank);
  REQUIRE_FALSE(status.hblank);  // h_ back to 0 at line start
}

TEST_CASE("PPU VBlank NMI latch arms at V=225, clears on read", "[unit][ppu]") {
  SNES snes;
  snes.Reset();
  Ppu& ppu = snes.GetPpu();

  // Before VBlank the latch is clear.
  REQUIRE_FALSE(ppu.QueryAndClearVblankNmiFlag(224U * 1364U));

  // First read after V=225 sees the latch high.
  constexpr TimeMasterT kStartOfV225 = 225U * 1364U;
  REQUIRE(ppu.QueryAndClearVblankNmiFlag(kStartOfV225));

  // Subsequent read inside the same VBlank returns clear — the prior read
  // consumed the latch and nothing has re-armed it.
  REQUIRE_FALSE(ppu.QueryAndClearVblankNmiFlag(kStartOfV225 + 1000));

  // After a full frame wraps (V=0 again), re-entering VBlank rearms.
  constexpr TimeMasterT kNextVblank = 262U * 1364U + 225U * 1364U;
  REQUIRE(ppu.QueryAndClearVblankNmiFlag(kNextVblank));
}

TEST_CASE("PPU VBlank NMI latch honors overscan start line", "[unit][ppu]") {
  SNES snes;
  snes.Reset();
  Ppu& ppu = snes.GetPpu();

  BusWrite(snes, sppu::regs::kSetini, sppu::regs::kSetiniOverscanMask,
           /*now=*/0);

  // Under overscan, V=225 no longer triggers NMI; V=240 does.
  REQUIRE_FALSE(ppu.QueryAndClearVblankNmiFlag(225U * 1364U));
  REQUIRE(ppu.QueryAndClearVblankNmiFlag(240U * 1364U));
}

TEST_CASE("PPU QueryHvbStatus honors SETINI overscan for VBlank start line",
          "[unit][ppu]") {
  SNES snes;
  snes.Reset();
  Ppu& ppu = snes.GetPpu();

  // Enable overscan before any dot emits so V=225..239 is still active.
  BusWrite(snes, sppu::regs::kSetini, sppu::regs::kSetiniOverscanMask,
           /*now=*/0);

  // At V=225 H=0 with overscan, VBlank should still be clear.
  constexpr TimeMasterT kStartOfV225 = 225U * 1364U;
  auto status = ppu.QueryHvbStatus(kStartOfV225);
  REQUIRE_FALSE(status.vblank);

  // At V=240 H=0 with overscan, VBlank becomes active.
  constexpr TimeMasterT kStartOfV240 = 240U * 1364U;
  status = ppu.QueryHvbStatus(kStartOfV240);
  REQUIRE(status.vblank);
}
