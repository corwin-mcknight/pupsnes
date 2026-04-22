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
  snes.scheduler->CatchUpDevice(ppu.GetDeviceId(), kFrameEnd);

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
  snes.scheduler->CatchUpDevice(ppu.GetDeviceId(), kFrame1End);
  REQUIRE(probe.count == 1);
  REQUIRE(ppu.GetField() == true);  // toggled at end-of-frame

  // Frame 2 has field=true → V=240 is the short 1360-cycle line. Total = 357364.
  constexpr TimeMasterT kFrame2End = kFrame1End + 261U * 1364U + 1360U;
  snes.scheduler->CatchUpDevice(ppu.GetDeviceId(), kFrame2End);
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
    snes.scheduler->CatchUpDevice(ppu.GetDeviceId(), kFrameEnd);

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
  snes.scheduler->CatchUpDevice(ppu.GetDeviceId(), kFrameEnd);

  REQUIRE(probe.count == 1);
  REQUIRE(ppu.IsForcedBlank());
  // Even the visible center is 0 under forced blank.
  const uint16_t center = ppu.GetFrontBuffer()[100U * sppu::regs::kFrameBufferWidth + 100U];
  REQUIRE(center == 0x0000);
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
