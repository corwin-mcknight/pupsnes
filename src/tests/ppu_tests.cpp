#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>

#include "pupsnes/core/scheduler.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/hw/sppu/ppu_regs.h"
#include "pupsnes/memory/systembus.h"

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
  BusWrite(snes, sppu::regs::kCgData, 0x34, /*now=*/1);  // low byte
  BusWrite(snes, sppu::regs::kCgData, 0xFF, /*now=*/2);  // high byte; bit 7 dropped

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
  // Driven bits: version=0x03 (bits 3:0), PAL=0 (bit 4), latch flag=0
  // (bit 6), field=0 on reset (bit 7). Only bit 5 is open-bus, so it
  // surfaces as 1 from the latch.
  // Expected: 0b 0 0 1 0 0011 = 0x23.
  REQUIRE(stat.data == 0x23);
  (void)ppu;
}

TEST_CASE("SLHV ($2137) read latches H/V into OPHCT/OPVCT and arms STAT78 flag", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  // Before any latch trigger: STAT78.bit6 must be 0.
  BusWrite(snes, 0x7E0000, 0x00, /*now=*/0);
  BusFollowResult pre = BusRead(snes, sppu::regs::kStat78, /*now=*/1);
  REQUIRE((pre.data & sppu::regs::kStat78LatchFlagMask) == 0);

  // Advance the PPU to a known dot position. SLHV read at master_time=4 lets
  // the bus catch-up emit dot (0,0); the PPU then sits at h=1, v=0 (h points
  // at the next dot to emit, which is the live beam position).
  (void)BusRead(snes, sppu::regs::kSlhv, /*now=*/4);
  REQUIRE(ppu.GetOphct() == 1U);
  REQUIRE(ppu.GetOpvct() == 0U);
  REQUIRE(ppu.GetHvLatchFlag());

  // 1st OPHCT read returns bits 7:0 (=1), 2nd returns bit 8 (=0, only
  // bit 0 driven; bits 7:1 come from open-bus).
  BusWrite(snes, 0x7E0000, 0x00, /*now=*/5);
  BusFollowResult oph_lo = BusRead(snes, sppu::regs::kOphct, /*now=*/6);
  REQUIRE(oph_lo.data == 0x01);
  BusFollowResult oph_hi = BusRead(snes, sppu::regs::kOphct, /*now=*/7);
  REQUIRE(oph_hi.data == 0x00);

  // Same for OPVCT (low=0, high=0).
  BusFollowResult opv_lo = BusRead(snes, sppu::regs::kOpvct, /*now=*/8);
  REQUIRE(opv_lo.data == 0x00);
  BusFollowResult opv_hi = BusRead(snes, sppu::regs::kOpvct, /*now=*/9);
  REQUIRE(opv_hi.data == 0x00);
}

TEST_CASE("OPHCT latches H counter values above 0xFF and exposes bit 8 on 2nd read", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  // Drive the PPU past H=255 inside a single scanline. Dots 0..255 all cost
  // 4 mcyc (H=323/327 are the only 6-cyc dots), so a SLHV read at
  // master_time = 256 * 4 = 1024 puts h_ at 256.
  (void)BusRead(snes, sppu::regs::kSlhv, /*now=*/1024);
  REQUIRE(ppu.GetOphct() == 256U);
  REQUIRE(ppu.GetOpvct() == 0U);

  // Consume the low byte first (a fully-driven read clobbers the bus latch
  // to 0x00, so we have to re-prime open-bus between the two reads).
  BusFollowResult oph_lo = BusRead(snes, sppu::regs::kOphct, /*now=*/1026);
  REQUIRE(oph_lo.data == 0x00);  // 256 & 0xFF
  // Re-prime open-bus to 0xFE so the 2nd-read masking is observable: only
  // bit 0 is driven by the device; bits 7:1 must surface from the latch.
  BusWrite(snes, 0x7E0000, 0xFE, /*now=*/1027);
  BusFollowResult oph_hi = BusRead(snes, sppu::regs::kOphct, /*now=*/1028);
  // bit 0 driven (=1 since 256 >> 8 == 1); bits 7:1 = 0xFE from latch.
  REQUIRE(oph_hi.data == 0xFF);
}

TEST_CASE("STAT78 read resets both OPHCT and OPVCT 1st/2nd flipflops and clears the latch flag", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  // Latch H/V and consume the OPHCT low-byte (flipflop now at "high").
  (void)BusRead(snes, sppu::regs::kSlhv, /*now=*/4);
  REQUIRE(ppu.GetHvLatchFlag());
  BusFollowResult low_first = BusRead(snes, sppu::regs::kOphct, /*now=*/5);
  REQUIRE(low_first.data == 0x01);  // h=1 low byte

  // STAT78 read: latch-flag bit set in returned data, then both flipflops
  // and the live flag clear.
  BusWrite(snes, 0x7E0000, 0x00, /*now=*/6);
  BusFollowResult stat = BusRead(snes, sppu::regs::kStat78, /*now=*/7);
  REQUIRE((stat.data & sppu::regs::kStat78LatchFlagMask) != 0);
  REQUIRE(!ppu.GetHvLatchFlag());

  // Subsequent STAT78 read shows the flag cleared.
  BusFollowResult stat2 = BusRead(snes, sppu::regs::kStat78, /*now=*/8);
  REQUIRE((stat2.data & sppu::regs::kStat78LatchFlagMask) == 0);

  // OPHCT next read is the low byte again (flipflop was reset).
  BusFollowResult low_again = BusRead(snes, sppu::regs::kOphct, /*now=*/9);
  REQUIRE(low_again.data == 0x01);
}

TEST_CASE("OPHCT and OPVCT have independent 1st/2nd flipflops", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();

  (void)BusRead(snes, sppu::regs::kSlhv, /*now=*/4);
  REQUIRE(ppu.GetOphct() == 1U);
  REQUIRE(ppu.GetOpvct() == 0U);

  // Toggle OPHCT to "expect high next" without touching OPVCT.
  BusFollowResult oph_lo = BusRead(snes, sppu::regs::kOphct, /*now=*/5);
  REQUIRE(oph_lo.data == 0x01);

  // OPVCT still returns the low byte first (independent flipflop).
  BusWrite(snes, 0x7E0000, 0x00, /*now=*/6);
  BusFollowResult opv_lo = BusRead(snes, sppu::regs::kOpvct, /*now=*/7);
  REQUIRE(opv_lo.data == 0x00);  // v=0 low byte

  // OPHCT next read is the high byte (its flipflop advanced).
  BusFollowResult oph_hi = BusRead(snes, sppu::regs::kOphct, /*now=*/8);
  REQUIRE(oph_hi.data == 0x00);  // h>>8 = 0
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
  BusWrite(snes, sppu::regs::kCgData, 0x1F, /*now=*/1);   // low byte: R=0x1F
  BusWrite(snes, sppu::regs::kCgData, 0x00, /*now=*/2);   // high byte: G=B=0
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

    std::memcpy(dest, ppu.GetFrontBuffer(), sppu::regs::kFrameBufferPixels * sizeof(uint16_t));
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

    const uint8_t vmain =
        static_cast<uint8_t>(sppu::regs::kVmainIncrementOnHighMask | (mode_bits << sppu::regs::kVmainTranslateShift));
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

TEST_CASE("PPU QueryHvbStatus honors SETINI overscan for VBlank start line", "[unit][ppu]") {
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

// ---------------------------------------------------------------------------
// Mode 1 BG rendering tests.
// ---------------------------------------------------------------------------
// All Mode 1 tests share a small set of helpers that drive VRAM/CGRAM via the
// real ports (through SystemBus, so the lazy-replay log is exercised end-to-
// end). Each test sets up state at low cycle counts (well before the first
// visible dot at master cycle 1452 = V=1 H=22) and then calls CatchUpTo past
// the end of the NTSC frame to render and swap buffers. After swap, the
// rendered frame is in the front buffer; BuildFrontView() points at the
// visible (256x224) sub-region.

namespace {

constexpr TimeMasterT kFrameEndNtsc = 262U * 1364U + 100U;

void SetVramAddress(SNES& snes, uint16_t word_addr, TimeMasterT& now) {
  BusWrite(snes, sppu::regs::kVmAddL, static_cast<uint8_t>(word_addr & 0xFFU), now++);
  BusWrite(snes, sppu::regs::kVmAddH, static_cast<uint8_t>((word_addr >> 8) & 0xFFU), now++);
}

void WriteVramWord(SNES& snes, uint16_t word_addr, uint16_t value, TimeMasterT& now) {
  SetVramAddress(snes, word_addr, now);
  BusWrite(snes, sppu::regs::kVmDataL, static_cast<uint8_t>(value & 0xFFU), now++);
  BusWrite(snes, sppu::regs::kVmDataH, static_cast<uint8_t>((value >> 8) & 0xFFU), now++);
}

void WriteCgramWord(SNES& snes, uint8_t cgadd, uint16_t color, TimeMasterT& now) {
  BusWrite(snes, sppu::regs::kCgAdd, cgadd, now++);
  BusWrite(snes, sppu::regs::kCgData, static_cast<uint8_t>(color & 0xFFU), now++);
  BusWrite(snes, sppu::regs::kCgData, static_cast<uint8_t>((color >> 8) & 0x7FU), now++);
}

// Write a 4bpp 8x8 tile pattern to VRAM at `char_word_base + char_index*16`
// (in words, since each 4bpp 8x8 tile is 16 words = 32 bytes). `pixels` is
// row-major, 8 bytes per row, each byte a 4-bit palette index in the LOW
// nibble (high nibble ignored). `pixels[r*8+c]` becomes column c of row r.
void WriteTile4bpp(SNES& snes, uint16_t char_word_base, uint16_t char_index, const uint8_t (&pixels)[64],
                   TimeMasterT& now) {
  const uint16_t base = static_cast<uint16_t>(char_word_base + char_index * 16U);
  // Planes 0/1 in words [0..7], planes 2/3 in words [8..15]. Bit 7 of each
  // plane byte is the leftmost pixel.
  for (uint16_t row = 0; row < 8; ++row) {
    uint8_t p0 = 0, p1 = 0, p2 = 0, p3 = 0;
    for (uint16_t col = 0; col < 8; ++col) {
      const uint8_t v = pixels[row * 8 + col] & 0x0FU;
      const uint8_t shift = static_cast<uint8_t>(7U - col);
      p0 |= static_cast<uint8_t>((v & 1U) << shift);
      p1 |= static_cast<uint8_t>(((v >> 1) & 1U) << shift);
      p2 |= static_cast<uint8_t>(((v >> 2) & 1U) << shift);
      p3 |= static_cast<uint8_t>(((v >> 3) & 1U) << shift);
    }
    WriteVramWord(snes, static_cast<uint16_t>(base + row), static_cast<uint16_t>(p0 | (p1 << 8U)), now);
    WriteVramWord(snes, static_cast<uint16_t>(base + 8U + row), static_cast<uint16_t>(p2 | (p3 << 8U)), now);
  }
}

// Set the OAM byte address. byte_addr must be even (the OAMADDL latch only
// captures bits 1..8 of byte_addr; bit 0 is implicitly 0). For odd-byte access
// in the high table, set the next-lower even byte and let port-side
// auto-increment carry you across.
void SetOamByteAddress(SNES& snes, uint16_t byte_addr, TimeMasterT& now) {
  BusWrite(snes, sppu::regs::kOamAddH, static_cast<uint8_t>((byte_addr >> 9U) & 0x01U), now++);
  BusWrite(snes, sppu::regs::kOamAddL, static_cast<uint8_t>((byte_addr >> 1U) & 0xFFU), now++);
}

// Configure OBJ N's 4-byte low-table entry (X low, Y, tile low, attributes).
// Other OBJs' bytes are not touched.
void WriteOamLowEntry(SNES& snes, uint8_t obj_index, uint8_t x_lo, uint8_t y, uint8_t tile_lo, uint8_t attr,
                      TimeMasterT& now) {
  SetOamByteAddress(snes, static_cast<uint16_t>(obj_index * 4U), now);
  BusWrite(snes, sppu::regs::kOamData, x_lo, now++);
  BusWrite(snes, sppu::regs::kOamData, y, now++);
  BusWrite(snes, sppu::regs::kOamData, tile_lo, now++);
  BusWrite(snes, sppu::regs::kOamData, attr, now++);
}

// Replace the entire byte at OAM high-table offset $200 + N (where N=0..31).
// Bit (obj%4)*2 holds X-high, bit (obj%4)*2 + 1 holds size (0=small, 1=large)
// for OBJs (group*4)..(group*4 + 3) where group = N. Use this when you need
// to set bits for a single OBJ's group; other OBJs in the same group get
// reset to 0/0 unless you set those bits explicitly.
void WriteOamHighGroupByte(SNES& snes, uint8_t group_index, uint8_t value, TimeMasterT& now) {
  SetOamByteAddress(snes, static_cast<uint16_t>(0x200U + group_index), now);
  BusWrite(snes, sppu::regs::kOamData, value, now++);
}

// Same shape, 2bpp variant — 8 words per tile.
void WriteTile2bpp(SNES& snes, uint16_t char_word_base, uint16_t char_index, const uint8_t (&pixels)[64],
                   TimeMasterT& now) {
  const uint16_t base = static_cast<uint16_t>(char_word_base + char_index * 8U);
  for (uint16_t row = 0; row < 8; ++row) {
    uint8_t p0 = 0, p1 = 0;
    for (uint16_t col = 0; col < 8; ++col) {
      const uint8_t v = pixels[row * 8 + col] & 0x03U;
      const uint8_t shift = static_cast<uint8_t>(7U - col);
      p0 |= static_cast<uint8_t>((v & 1U) << shift);
      p1 |= static_cast<uint8_t>(((v >> 1) & 1U) << shift);
    }
    WriteVramWord(snes, static_cast<uint16_t>(base + row), static_cast<uint16_t>(p0 | (p1 << 8U)), now);
  }
}

}  // namespace

TEST_CASE("Mode 1 BG1 4bpp tile renders to the framebuffer with palette colors", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  // Drop forced blank, full brightness so BrightnessScale is identity.
  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  // VMAIN: word increment (+1), no translation, increment after VMDATAH.
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);

  // Palette: backdrop=black, color1=red, color2=green, color5=white.
  WriteCgramWord(snes, 0U, 0x0000U, now);
  WriteCgramWord(snes, 1U, 0x001FU, now);  // BGR555 red
  WriteCgramWord(snes, 2U, 0x03E0U, now);  // green
  WriteCgramWord(snes, 5U, 0x7FFFU, now);  // white

  // Tile 0: row 0 alternates between color 1 and color 2; rest is color 5.
  uint8_t tile0[64] = {};
  for (uint16_t c = 0; c < 8; ++c) tile0[c] = (c & 1U) ? 2U : 1U;
  for (uint16_t i = 8; i < 64; ++i) tile0[i] = 5U;
  // Char base 0x1000 words = 0x2000 bytes (BG1 nibble = 1).
  WriteTile4bpp(snes, /*char_word_base=*/0x1000U, /*char_index=*/0, tile0, now);

  // Tilemap entry (0,0): char 0, palette 0, no flip, no priority.
  WriteVramWord(snes, /*word_addr=*/0x0000U, /*value=*/0x0000U, now);

  // BGMODE=1, BG1 8x8. BG1SC: tilemap base 0x0000, layout 32x32.
  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);
  BusWrite(snes, sppu::regs::kBg1Sc, 0x00U, now++);
  // BG12NBA: BG1 char base nibble = 1 → 0x1000 words.
  BusWrite(snes, sppu::regs::kBg12Nba, 0x01U, now++);
  // Scroll = 0 on both axes (write twice for the latch).
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  // TM: enable BG1 only.
  BusWrite(snes, sppu::regs::kTm, sppu::regs::kTmBg1Mask, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();

  // Row 0 columns 0..7 alternate red / green per tile0 pattern.
  REQUIRE(view.pixels[0] == 0x001FU);
  REQUIRE(view.pixels[1] == 0x03E0U);
  REQUIRE(view.pixels[2] == 0x001FU);
  REQUIRE(view.pixels[3] == 0x03E0U);
  REQUIRE(view.pixels[6] == 0x001FU);
  REQUIRE(view.pixels[7] == 0x03E0U);
  // Row 1+ should be white (color 5).
  REQUIRE(view.pixels[1U * view.stride + 0U] == 0x7FFFU);
  REQUIRE(view.pixels[7U * view.stride + 7U] == 0x7FFFU);
}

TEST_CASE("Mode 1 BG1 hflip + vflip mirror the 8x8 tile pattern", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);
  WriteCgramWord(snes, 1U, 0x001FU, now);  // red
  WriteCgramWord(snes, 2U, 0x03E0U, now);  // green
  WriteCgramWord(snes, 3U, 0x7C00U, now);  // blue

  // Tile pattern marks (col=0,row=0)=red, (col=7,row=0)=green, (col=0,row=7)=blue.
  uint8_t tile[64] = {};
  tile[0 * 8 + 0] = 1;
  tile[0 * 8 + 7] = 2;
  tile[7 * 8 + 0] = 3;
  WriteTile4bpp(snes, 0x1000U, 0, tile, now);

  // Tilemap (0,0) = char 0, hflip + vflip set (bits 14, 15).
  WriteVramWord(snes, 0x0000U, 0xC000U, now);

  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);
  BusWrite(snes, sppu::regs::kBg1Sc, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg12Nba, 0x01U, now++);
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg2Sc, 0x00U, now++);  // disable BG2 (default tile 0 → backdrop via TM)
  BusWrite(snes, sppu::regs::kTm, sppu::regs::kTmBg1Mask, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();

  // After hflip+vflip, (col=0,row=0) source maps to rendered (col=7,row=7).
  REQUIRE(view.pixels[7U * view.stride + 7U] == 0x001FU);  // red, was source (0,0)
  REQUIRE(view.pixels[7U * view.stride + 0U] == 0x03E0U);  // green, was source (7,0)
  REQUIRE(view.pixels[0U * view.stride + 7U] == 0x7C00U);  // blue, was source (0,7)
}

TEST_CASE("Mode 1 BG1 16x16 tile picks the right 8x8 sub-tile", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);
  WriteCgramWord(snes, 1U, 0x001FU, now);  // red    (TL filler)
  WriteCgramWord(snes, 2U, 0x03E0U, now);  // green  (TR filler)
  WriteCgramWord(snes, 3U, 0x7C00U, now);  // blue   (BL filler)
  WriteCgramWord(snes, 4U, 0x7FFFU, now);  // white  (BR filler)

  // Sub-tile char indices: TL=0, TR=1, BL=0x10, BR=0x11.
  auto write_solid = [&](uint16_t char_idx, uint8_t color) {
    uint8_t buf[64];
    for (uint16_t i = 0; i < 64; ++i) buf[i] = color;
    WriteTile4bpp(snes, 0x1000U, char_idx, buf, now);
  };
  write_solid(0x0000U, 1U);  // TL = red
  write_solid(0x0001U, 2U);  // TR = green
  write_solid(0x0010U, 3U);  // BL = blue
  write_solid(0x0011U, 4U);  // BR = white

  // Tilemap (0,0) = char 0, no flip.
  WriteVramWord(snes, 0x0000U, 0x0000U, now);

  // BGMODE=1, BG1 16x16 tile size (bit 4 set).
  BusWrite(snes, sppu::regs::kBgmode, static_cast<uint8_t>(0x01U | sppu::regs::kBgmodeBg1TileSizeMask), now++);
  BusWrite(snes, sppu::regs::kBg1Sc, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg12Nba, 0x01U, now++);
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kTm, sppu::regs::kTmBg1Mask, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();

  // TL quadrant (cols 0..7, rows 0..7) = red.
  REQUIRE(view.pixels[0U * view.stride + 0U] == 0x001FU);
  REQUIRE(view.pixels[7U * view.stride + 7U] == 0x001FU);
  // TR (cols 8..15, rows 0..7) = green.
  REQUIRE(view.pixels[0U * view.stride + 8U] == 0x03E0U);
  REQUIRE(view.pixels[7U * view.stride + 15U] == 0x03E0U);
  // BL (cols 0..7, rows 8..15) = blue.
  REQUIRE(view.pixels[8U * view.stride + 0U] == 0x7C00U);
  REQUIRE(view.pixels[15U * view.stride + 7U] == 0x7C00U);
  // BR (cols 8..15, rows 8..15) = white.
  REQUIRE(view.pixels[8U * view.stride + 8U] == 0x7FFFU);
  REQUIRE(view.pixels[15U * view.stride + 15U] == 0x7FFFU);
}

TEST_CASE("Mode 1 64x32 layout fetches second tilemap screen past the seam", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);
  WriteCgramWord(snes, 1U, 0x001FU, now);  // red    (tile 0)
  WriteCgramWord(snes, 2U, 0x03E0U, now);  // green  (tile 1)

  // Tile 0 = solid red, Tile 1 = solid green.
  uint8_t solid_red[64];
  for (uint16_t i = 0; i < 64; ++i) solid_red[i] = 1U;
  uint8_t solid_green[64];
  for (uint16_t i = 0; i < 64; ++i) solid_green[i] = 2U;
  WriteTile4bpp(snes, 0x1000U, 0, solid_red, now);
  WriteTile4bpp(snes, 0x1000U, 1, solid_green, now);

  // Left screen at word 0x0000 — fill row 0 entries with char 0 (red).
  // Right screen at word 0x0400 — fill row 0 entries with char 1 (green).
  // Only need entry 0 of each, but write a couple to be sure.
  for (uint16_t c = 0; c < 4; ++c) {
    WriteVramWord(snes, static_cast<uint16_t>(0x0000U + c), 0x0000U, now);  // char 0
    WriteVramWord(snes, static_cast<uint16_t>(0x0400U + c), 0x0001U, now);  // char 1
  }

  // BGMODE=1, BG1 8x8.
  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);
  // BG1SC: tilemap base 0, layout = 64x32 (bits 1:0 = 1).
  BusWrite(snes, sppu::regs::kBg1Sc, 0x01U, now++);
  BusWrite(snes, sppu::regs::kBg12Nba, 0x01U, now++);

  // Set BG1 H scroll = 32*8 = 256 so the visible window starts in the right
  // (second) screen. Scroll value 256 = 0x100; write low=0x00, high=0x01.
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x01U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kTm, sppu::regs::kTmBg1Mask, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();

  // With H scroll = 256, screen column 0 maps to tile column 32 (in the
  // second 32-wide screen, char 1 = green).
  REQUIRE(view.pixels[0U * view.stride + 0U] == 0x03E0U);
}

TEST_CASE("BG scroll write-twice latch matches the fullsnes BG_old formula", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  // BG1HOFS first write 0x80 (Curr=0x80, Prev_old=0, Reg_old=0):
  //   BG1HOFS = (0x80<<8) | (0&~7) | ((0>>8)&7) = 0x8000 → masked to 10 bits = 0x000.
  //   Prev = 0x80.
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x80U, now++);

  // BG1HOFS second write 0x05 (Curr=0x05, Prev_old=0x80, Reg_old=0x000):
  //   BG1HOFS = (0x05<<8) | (0x80 & ~7) | ((0x000>>8)&7)
  //           = 0x0500 | 0x80 | 0 = 0x0580 → masked = 0x180.
  //   Prev = 0x05.
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x05U, now++);

  // BG1VOFS first write 0x12 (Curr=0x12, Prev_old=0x05):
  //   BG1VOFS = (0x12<<8) | 0x05 = 0x1205 → masked = 0x005.
  //   Prev = 0x12.
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x12U, now++);

  // BG1VOFS second write 0x07 (Curr=0x07, Prev_old=0x12):
  //   BG1VOFS = (0x07<<8) | 0x12 = 0x0712 → masked = 0x312.
  //   Prev = 0x07.
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x07U, now++);

  // Force a catch-up so the log replays.
  (void)BusRead(snes, sppu::regs::kStat77, now++);

  REQUIRE(ppu.GetBgHofs(0) == 0x180U);
  REQUIRE(ppu.GetBgVofs(0) == 0x312U);
}

TEST_CASE("BG1HOFS smooth-scroll preserves bit 2 of the low byte", "[unit][ppu]") {
  // Regression: SMW writes BG1HOFS as low-then-high every frame. When the
  // game increments scroll by one pixel per frame, the low byte sweeps
  // 0..255. Earlier code stored bg_hofs_ pre-masked to 10 bits, which
  // discarded bit 10 of the raw shift-in — that bit is exactly bit 2 of
  // the previously written "Curr". On the very next write, the fullsnes
  // "((BGnHOFS_old>>8) & 7)" feedback term then yielded only 2 bits of
  // the previous Curr, clearing bit 2 of the final effective offset.
  //
  // Observable symptom in SMW: BG visibly scrolled backwards 4 pixels
  // when H_low crossed bit 2, then snapped forward 8 when bit 3 caught
  // up. This test pins down the per-pixel offset for low bytes 0..15
  // (always with high byte = 0).
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  for (uint8_t lo = 0; lo < 16U; ++lo) {
    BusWrite(snes, sppu::regs::kBg1Hofs, lo, now++);
    BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
    (void)BusRead(snes, sppu::regs::kStat77, now++);
    INFO("low byte = " << static_cast<int>(lo));
    REQUIRE(ppu.GetBgHofs(0) == static_cast<uint16_t>(lo));
  }
}

TEST_CASE("Mode 1 BG3 priority bit sends BG3 prio-1 above BG1 prio-1", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);
  WriteCgramWord(snes, 1U, 0x001FU, now);   // red    (BG1)
  WriteCgramWord(snes, 16U, 0x03E0U, now);  // BG3 palette group 0, color... actually
  // For BG3 (2bpp), palette group N color C → CGRAM index (N << 2) | C.
  // We want palette group 4 (so index = 4*4 = 16) color 1 → green at CGRAM[17].
  WriteCgramWord(snes, 17U, 0x03E0U, now);  // green (BG3 palette 4 color 1)

  // BG1 char base 0x1000 words; BG3 char base 0x2000 words.
  // BG1 tile 0 = solid color 1 (red).
  uint8_t solid_red[64];
  for (uint16_t i = 0; i < 64; ++i) solid_red[i] = 1U;
  WriteTile4bpp(snes, 0x1000U, 0, solid_red, now);
  // BG3 tile 0 = solid 2bpp color 1.
  uint8_t solid_2bpp_1[64];
  for (uint16_t i = 0; i < 64; ++i) solid_2bpp_1[i] = 1U;
  WriteTile2bpp(snes, 0x2000U, 0, solid_2bpp_1, now);

  // BG1 tilemap at word 0x3000 — char 0, palette 0, priority=1.
  WriteVramWord(snes, 0x3000U, 0x2000U, now);  // bit 13 = priority
  // BG3 tilemap at word 0x3400 — char 0, palette 4, priority=1.
  // palette_group field = bits 10..12 = 4 → 4 << 10 = 0x1000.
  WriteVramWord(snes, 0x3400U, static_cast<uint16_t>(0x2000U | 0x1000U), now);

  // BGMODE=1, BG3 priority bit set.
  BusWrite(snes, sppu::regs::kBgmode, static_cast<uint8_t>(0x01U | sppu::regs::kBgmodeBg3PriorityMask), now++);
  // BG1SC tilemap base = 0x3000 → (data & 0xFC) << 8 = 0x3000 → data = 0x30.
  BusWrite(snes, sppu::regs::kBg1Sc, 0x30U, now++);
  // BG3SC tilemap base = 0x3400 → data = 0x34.
  BusWrite(snes, sppu::regs::kBg3Sc, 0x34U, now++);
  // BG12NBA: BG1 char base nibble = 1.
  BusWrite(snes, sppu::regs::kBg12Nba, 0x01U, now++);
  // BG34NBA: BG3 char base nibble = 2.
  BusWrite(snes, sppu::regs::kBg34Nba, 0x02U, now++);
  // No scroll.
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg3Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg3Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg3Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg3Vofs, 0x00U, now++);
  // TM: enable BG1 + BG3.
  BusWrite(snes, sppu::regs::kTm, static_cast<uint8_t>(sppu::regs::kTmBg1Mask | sppu::regs::kTmBg3Mask), now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();

  // With BG3 priority bit set, BG3 prio-1 wins over BG1 prio-1 → green.
  REQUIRE(view.pixels[0U * view.stride + 0U] == 0x03E0U);
}

TEST_CASE("Mode 1 TM mask disables BG1, exposes BG2 contribution", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);
  WriteCgramWord(snes, 1U, 0x001FU, now);  // red   (BG1)
  WriteCgramWord(snes, 2U, 0x03E0U, now);  // green (BG2)

  // BG1 char base 0x1000 (nibble 1), BG2 char base 0x2000 (nibble 2). Both 4bpp.
  uint8_t bg1_red[64];
  for (uint16_t i = 0; i < 64; ++i) bg1_red[i] = 1U;
  uint8_t bg2_green[64];
  for (uint16_t i = 0; i < 64; ++i) bg2_green[i] = 2U;
  WriteTile4bpp(snes, 0x1000U, 0, bg1_red, now);
  WriteTile4bpp(snes, 0x2000U, 0, bg2_green, now);

  // BG1 tilemap at word 0x3000, char 0 priority 0.
  WriteVramWord(snes, 0x3000U, 0x0000U, now);
  // BG2 tilemap at word 0x3400, char 0 priority 0.
  WriteVramWord(snes, 0x3400U, 0x0000U, now);

  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);
  BusWrite(snes, sppu::regs::kBg1Sc, 0x30U, now++);  // 0x3000 word base
  BusWrite(snes, sppu::regs::kBg2Sc, 0x34U, now++);  // 0x3400 word base
  // BG12NBA: BG1 nibble=1 → 0x1000, BG2 nibble=2 → 0x2000.
  BusWrite(snes, sppu::regs::kBg12Nba, static_cast<uint8_t>(0x01U | (0x02U << 4U)), now++);
  // No scroll for either BG.
  for (uint16_t reg : {sppu::regs::kBg1Hofs, sppu::regs::kBg1Hofs, sppu::regs::kBg1Vofs, sppu::regs::kBg1Vofs,
                       sppu::regs::kBg2Hofs, sppu::regs::kBg2Hofs, sppu::regs::kBg2Vofs, sppu::regs::kBg2Vofs}) {
    BusWrite(snes, reg, 0x00U, now++);
  }
  // TM: disable BG1, enable BG2.
  BusWrite(snes, sppu::regs::kTm, sppu::regs::kTmBg2Mask, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();

  // BG1 disabled in TM → BG2's green wins.
  REQUIRE(view.pixels[0U * view.stride + 0U] == 0x03E0U);
}

TEST_CASE("Mode 1 transparent BG pixels fall through to backdrop", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x7C1FU, now);  // backdrop = magenta
  WriteCgramWord(snes, 1U, 0x001FU, now);  // (unused) red

  // BG1 tile 0 = all transparent (color index 0 everywhere).
  uint8_t empty[64] = {};
  WriteTile4bpp(snes, 0x1000U, 0, empty, now);
  WriteVramWord(snes, 0x0000U, 0x0000U, now);

  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);
  BusWrite(snes, sppu::regs::kBg1Sc, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg12Nba, 0x01U, now++);
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kTm, sppu::regs::kTmBg1Mask, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();

  // BG1 transparent everywhere → backdrop magenta.
  REQUIRE(view.pixels[0U * view.stride + 0U] == 0x7C1FU);
  REQUIRE(view.pixels[10U * view.stride + 200U] == 0x7C1FU);
}

TEST_CASE("Mode 0 BG1 2bpp tile uses CGRAM region 0", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);  // backdrop
  WriteCgramWord(snes, 1U, 0x001FU, now);  // BG1 color 1 = red

  // BG1 char base 0x1000 words. Solid 2bpp color 1 tile.
  uint8_t solid_1[64];
  for (uint16_t i = 0; i < 64; ++i) solid_1[i] = 1U;
  WriteTile2bpp(snes, 0x1000U, 0, solid_1, now);
  WriteVramWord(snes, 0x0000U, 0x0000U, now);  // tilemap (0,0)=char 0

  BusWrite(snes, sppu::regs::kBgmode, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Sc, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg12Nba, 0x01U, now++);
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kTm, sppu::regs::kTmBg1Mask, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();
  REQUIRE(view.pixels[0U * view.stride + 0U] == 0x001FU);
}

TEST_CASE("Mode 0 BG2 reads palette from CGRAM region +32", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);
  // BG2 base = 32. Color 1 in palette group 0 lives at CGRAM[33].
  WriteCgramWord(snes, 33U, 0x03E0U, now);  // green

  // BG2 char base 0x2000 words.
  uint8_t solid_1[64];
  for (uint16_t i = 0; i < 64; ++i) solid_1[i] = 1U;
  WriteTile2bpp(snes, 0x2000U, 0, solid_1, now);
  // BG2 tilemap at word 0x0400.
  WriteVramWord(snes, 0x0400U, 0x0000U, now);

  BusWrite(snes, sppu::regs::kBgmode, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg2Sc, 0x04U, now++);                            // tilemap base 0x0400
  BusWrite(snes, sppu::regs::kBg12Nba, static_cast<uint8_t>(2U << 4), now++);  // BG2 nibble = 2
  BusWrite(snes, sppu::regs::kBg2Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg2Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg2Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg2Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kTm, sppu::regs::kTmBg2Mask, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();
  REQUIRE(view.pixels[0U * view.stride + 0U] == 0x03E0U);
}

TEST_CASE("Mode 0 BG3 reads palette from CGRAM region +64", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);
  WriteCgramWord(snes, 65U, 0x7C00U, now);  // BG3 color 1 = blue at CGRAM[64+1]

  uint8_t solid_1[64];
  for (uint16_t i = 0; i < 64; ++i) solid_1[i] = 1U;
  WriteTile2bpp(snes, 0x3000U, 0, solid_1, now);
  WriteVramWord(snes, 0x0800U, 0x0000U, now);

  BusWrite(snes, sppu::regs::kBgmode, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg3Sc, 0x08U, now++);                       // tilemap base 0x0800
  BusWrite(snes, sppu::regs::kBg34Nba, static_cast<uint8_t>(3U), now++);  // BG3 nibble = 3
  BusWrite(snes, sppu::regs::kBg3Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg3Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg3Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg3Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kTm, sppu::regs::kTmBg3Mask, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();
  REQUIRE(view.pixels[0U * view.stride + 0U] == 0x7C00U);
}

TEST_CASE("Mode 0 BG4 reads palette from CGRAM region +96", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);
  WriteCgramWord(snes, 97U, 0x7FFFU, now);  // BG4 color 1 = white at CGRAM[96+1]

  uint8_t solid_1[64];
  for (uint16_t i = 0; i < 64; ++i) solid_1[i] = 1U;
  WriteTile2bpp(snes, 0x4000U, 0, solid_1, now);
  WriteVramWord(snes, 0x0C00U, 0x0000U, now);

  BusWrite(snes, sppu::regs::kBgmode, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg4Sc, 0x0CU, now++);                            // tilemap base 0x0C00
  BusWrite(snes, sppu::regs::kBg34Nba, static_cast<uint8_t>(4U << 4), now++);  // BG4 nibble = 4
  BusWrite(snes, sppu::regs::kBg4Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg4Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg4Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg4Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kTm, sppu::regs::kTmBg4Mask, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();
  REQUIRE(view.pixels[0U * view.stride + 0U] == 0x7FFFU);
}

TEST_CASE("Mode 0 priority: BG2 prio-1 wins over BG1 prio-0", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);
  WriteCgramWord(snes, 1U, 0x001FU, now);   // BG1 color 1 = red
  WriteCgramWord(snes, 33U, 0x03E0U, now);  // BG2 color 1 = green (CGRAM[33] = +32 + 1)

  uint8_t solid_1[64];
  for (uint16_t i = 0; i < 64; ++i) solid_1[i] = 1U;
  WriteTile2bpp(snes, 0x1000U, 0, solid_1, now);  // BG1 tile
  WriteTile2bpp(snes, 0x2000U, 0, solid_1, now);  // BG2 tile

  // BG1 tilemap at 0x0000, char 0, priority 0.
  WriteVramWord(snes, 0x0000U, 0x0000U, now);
  // BG2 tilemap at 0x0400, char 0, priority 1.
  WriteVramWord(snes, 0x0400U, 0x2000U, now);

  BusWrite(snes, sppu::regs::kBgmode, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Sc, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg2Sc, 0x04U, now++);
  BusWrite(snes, sppu::regs::kBg12Nba, static_cast<uint8_t>(0x01U | (0x02U << 4)), now++);
  for (uint16_t reg : {sppu::regs::kBg1Hofs, sppu::regs::kBg1Hofs, sppu::regs::kBg1Vofs, sppu::regs::kBg1Vofs,
                       sppu::regs::kBg2Hofs, sppu::regs::kBg2Hofs, sppu::regs::kBg2Vofs, sppu::regs::kBg2Vofs}) {
    BusWrite(snes, reg, 0x00U, now++);
  }
  BusWrite(snes, sppu::regs::kTm, static_cast<uint8_t>(sppu::regs::kTmBg1Mask | sppu::regs::kTmBg2Mask), now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();
  // BG2 prio-1 sits above BG1 prio-0 in Mode 0's eight-slot order → green.
  REQUIRE(view.pixels[0U * view.stride + 0U] == 0x03E0U);
}

TEST_CASE("Mode 0 priority: BG1 prio-0 wins over BG2 prio-0", "[unit][ppu]") {
  // Within the same priority class, BG1 sits above BG2 (slot order:
  // BG1.h, BG2.h, BG3.h, BG4.h, BG1.l, BG2.l, BG3.l, BG4.l).
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);
  WriteCgramWord(snes, 1U, 0x001FU, now);   // BG1 red
  WriteCgramWord(snes, 33U, 0x03E0U, now);  // BG2 green

  uint8_t solid_1[64];
  for (uint16_t i = 0; i < 64; ++i) solid_1[i] = 1U;
  WriteTile2bpp(snes, 0x1000U, 0, solid_1, now);
  WriteTile2bpp(snes, 0x2000U, 0, solid_1, now);
  WriteVramWord(snes, 0x0000U, 0x0000U, now);  // BG1 prio 0
  WriteVramWord(snes, 0x0400U, 0x0000U, now);  // BG2 prio 0

  BusWrite(snes, sppu::regs::kBgmode, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Sc, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg2Sc, 0x04U, now++);
  BusWrite(snes, sppu::regs::kBg12Nba, static_cast<uint8_t>(0x01U | (0x02U << 4)), now++);
  for (uint16_t reg : {sppu::regs::kBg1Hofs, sppu::regs::kBg1Hofs, sppu::regs::kBg1Vofs, sppu::regs::kBg1Vofs,
                       sppu::regs::kBg2Hofs, sppu::regs::kBg2Hofs, sppu::regs::kBg2Vofs, sppu::regs::kBg2Vofs}) {
    BusWrite(snes, reg, 0x00U, now++);
  }
  BusWrite(snes, sppu::regs::kTm, static_cast<uint8_t>(sppu::regs::kTmBg1Mask | sppu::regs::kTmBg2Mask), now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();
  REQUIRE(view.pixels[0U * view.stride + 0U] == 0x001FU);  // BG1 red wins
}

TEST_CASE("Unimplemented BG modes still emit backdrop only", "[unit][ppu]") {
  // Modes 2..7 don't have renderers yet — they should always fall through
  // to the backdrop path regardless of how BG state is configured.
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x03E0U, now);  // backdrop = green
  WriteCgramWord(snes, 1U, 0x001FU, now);  // (would-be BG color)

  uint8_t solid_red[64];
  for (uint16_t i = 0; i < 64; ++i) solid_red[i] = 1U;
  WriteTile4bpp(snes, 0x1000U, 0, solid_red, now);
  WriteVramWord(snes, 0x0000U, 0x0000U, now);

  // BGMODE = 2 (offset-per-tile mode, not yet implemented).
  BusWrite(snes, sppu::regs::kBgmode, 0x02U, now++);
  BusWrite(snes, sppu::regs::kBg1Sc, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg12Nba, 0x01U, now++);
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kTm, sppu::regs::kTmBg1Mask, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();

  REQUIRE(view.pixels[0U * view.stride + 0U] == 0x03E0U);
  REQUIRE(view.pixels[100U * view.stride + 100U] == 0x03E0U);
}

// ---------------------------------------------------------------------------
// OBJ (sprite) tests.
// ---------------------------------------------------------------------------
// All OBJ tests follow the same shape: enable Mode 1 with no BGs (so OBJ
// rendering is the only contribution above backdrop), set up OBSEL, write
// sprite tile graphics into VRAM, populate OAM, enable OBJ in TM, and assert
// pixels in the framebuffer match expected sprite output.
//
// v1 walks every OAM entry per pixel; the 32-OBJ / 34-tile per-line cap is
// deferred. Tests can rely on default-zero OBJs being effectively transparent
// when their tile index is set to a tile whose VRAM data is zero.

TEST_CASE("OBJ basic 8x8 sprite renders at sprite position", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);     // backdrop = black
  WriteCgramWord(snes, 0x81U, 0x001FU, now);  // OBJ palette 0 color 1 = red

  // OBJ tile graphics at region 0 (OBSEL name base = 0). Use tile index 1 so
  // OBJs 1..127 (defaulting to tile 0 with zero VRAM) stay transparent.
  uint8_t solid_red[64];
  for (uint16_t i = 0; i < 64; ++i) solid_red[i] = 1U;
  WriteTile4bpp(snes, 0x0000U, 1U, solid_red, now);

  // OBSEL: size pair 0 (small=8x8, large=16x16), name base 0, no gap.
  BusWrite(snes, sppu::regs::kObsel, 0x00U, now++);
  // OBJ 0: x=10, y=20, tile=1, attr palette=0, no flip, priority=0.
  WriteOamLowEntry(snes, 0U, 10U, 20U, 1U, 0x00U, now);

  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);
  BusWrite(snes, sppu::regs::kTm, sppu::regs::kTmObjMask, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();

  // Sprite covers screen rows 20..27, cols 10..17.
  REQUIRE(view.pixels[20U * view.stride + 10U] == 0x001FU);
  REQUIRE(view.pixels[27U * view.stride + 17U] == 0x001FU);
  // Just outside the sprite → backdrop.
  REQUIRE(view.pixels[19U * view.stride + 10U] == 0x0000U);
  REQUIRE(view.pixels[20U * view.stride + 9U] == 0x0000U);
  REQUIRE(view.pixels[28U * view.stride + 10U] == 0x0000U);
  REQUIRE(view.pixels[20U * view.stride + 18U] == 0x0000U);
}

TEST_CASE("OBJ hflip + vflip mirror the 8x8 sprite", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);
  WriteCgramWord(snes, 0x81U, 0x001FU, now);  // red
  WriteCgramWord(snes, 0x82U, 0x03E0U, now);  // green
  WriteCgramWord(snes, 0x83U, 0x7C00U, now);  // blue

  // Sprite tile 1: top-left=red(1), top-right=green(2), bottom-left=blue(3).
  uint8_t tile[64] = {};
  tile[0 * 8 + 0] = 1;
  tile[0 * 8 + 7] = 2;
  tile[7 * 8 + 0] = 3;
  WriteTile4bpp(snes, 0x0000U, 1U, tile, now);

  BusWrite(snes, sppu::regs::kObsel, 0x00U, now++);
  // attr bit 6 = X-flip, bit 7 = Y-flip → 0xC0.
  WriteOamLowEntry(snes, 0U, 30U, 40U, 1U, 0xC0U, now);

  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);
  BusWrite(snes, sppu::regs::kTm, sppu::regs::kTmObjMask, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();

  // After hflip+vflip, source (0,0) maps to rendered (7,7) within the sprite.
  REQUIRE(view.pixels[(40U + 7U) * view.stride + (30U + 7U)] == 0x001FU);  // red
  REQUIRE(view.pixels[(40U + 7U) * view.stride + (30U + 0U)] == 0x03E0U);  // green
  REQUIRE(view.pixels[(40U + 0U) * view.stride + (30U + 7U)] == 0x7C00U);  // blue
}

TEST_CASE("OBJ 16x16 large sprite picks the right 8x8 sub-tile", "[unit][ppu]") {
  // Size pair 0: small=8x8, large=16x16. Set OBJ 0 large=1.
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);
  WriteCgramWord(snes, 0x81U, 0x001FU, now);  // TL = red
  WriteCgramWord(snes, 0x82U, 0x03E0U, now);  // TR = green
  WriteCgramWord(snes, 0x83U, 0x7C00U, now);  // BL = blue
  WriteCgramWord(snes, 0x84U, 0x7FFFU, now);  // BR = white

  // Sub-tile char indices for OBJ tile N=2: TL=2, TR=3, BL=2+0x10=0x12, BR=0x13.
  // (OBJ tile arithmetic carries within row low nibble only; with tile 2
  // there's plenty of room before any wrap.)
  auto fill_solid = [&](uint16_t char_idx, uint8_t color) {
    uint8_t buf[64];
    for (uint16_t i = 0; i < 64; ++i) buf[i] = color;
    WriteTile4bpp(snes, 0x0000U, char_idx, buf, now);
  };
  fill_solid(0x02U, 1U);  // TL = red
  fill_solid(0x03U, 2U);  // TR = green
  fill_solid(0x12U, 3U);  // BL = blue
  fill_solid(0x13U, 4U);  // BR = white

  BusWrite(snes, sppu::regs::kObsel, 0x00U, now++);
  WriteOamLowEntry(snes, 0U, 50U, 60U, 0x02U, 0x00U, now);
  // Set OBJ 0 large bit (size = 1 in high table).
  WriteOamHighGroupByte(snes, 0U, 0x02U, now);  // bit 1 = OBJ 0 size

  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);
  BusWrite(snes, sppu::regs::kTm, sppu::regs::kTmObjMask, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();

  // Expect a 16x16 sprite at (50, 60). Quadrants:
  REQUIRE(view.pixels[60U * view.stride + 50U] == 0x001FU);                  // TL red
  REQUIRE(view.pixels[60U * view.stride + 58U] == 0x03E0U);                  // TR green
  REQUIRE(view.pixels[68U * view.stride + 50U] == 0x7C00U);                  // BL blue
  REQUIRE(view.pixels[68U * view.stride + 58U] == 0x7FFFU);                  // BR white
  REQUIRE(view.pixels[(60U + 15U) * view.stride + (50U + 15U)] == 0x7FFFU);  // bottom-right corner
}

TEST_CASE("OBJ priority 3 renders above BG1 priority-1 tile in Mode 1", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);
  WriteCgramWord(snes, 1U, 0x001FU, now);     // BG1 color 1 = red
  WriteCgramWord(snes, 0x81U, 0x03E0U, now);  // OBJ palette 0 color 1 = green

  uint8_t solid_red[64];
  for (uint16_t i = 0; i < 64; ++i) solid_red[i] = 1U;
  uint8_t solid_green[64];
  for (uint16_t i = 0; i < 64; ++i) solid_green[i] = 1U;
  WriteTile4bpp(snes, 0x1000U, 0U, solid_red, now);    // BG1 char 0 = red
  WriteTile4bpp(snes, 0x4000U, 1U, solid_green, now);  // OBJ tile 1 = green (will use OBJ palette)

  // BG1 tilemap (0,0): char 0, palette 0, priority=1 (bit 13).
  WriteVramWord(snes, 0x0000U, 0x2000U, now);

  // OBSEL: name base 4 → OBJ region 0 starts at word 4*0x2000 = 0x8000... wait,
  // (OBSEL & 7) << 13 with OBSEL=4 → 4<<13 = 0x8000, masked to 15 bits = 0.
  // Try OBSEL=2: 2<<13 = 0x4000. ✓
  BusWrite(snes, sppu::regs::kObsel, 0x02U, now++);
  // OBJ 0: x=20, y=30, tile=1, attr priority=3 (bits 5:4 = 0b11), palette 0.
  WriteOamLowEntry(snes, 0U, 20U, 30U, 1U, 0x30U, now);

  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);
  BusWrite(snes, sppu::regs::kBg1Sc, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg12Nba, 0x01U, now++);
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kTm, static_cast<uint8_t>(sppu::regs::kTmBg1Mask | sppu::regs::kTmObjMask), now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();

  // At (20, 30): both BG1.h (red) and OBJ.3 (green) cover this pixel.
  // Mode 1 priority order starts with OBJ.3 → green wins.
  REQUIRE(view.pixels[30U * view.stride + 20U] == 0x03E0U);
  // Outside sprite, BG1 is everywhere → red.
  REQUIRE(view.pixels[100U * view.stride + 100U] == 0x001FU);
}

TEST_CASE("OBJ priority 0 renders below BG1 priority-1 tile in Mode 1", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);
  WriteCgramWord(snes, 1U, 0x001FU, now);     // BG1 red
  WriteCgramWord(snes, 0x81U, 0x03E0U, now);  // OBJ green

  uint8_t solid_1[64];
  for (uint16_t i = 0; i < 64; ++i) solid_1[i] = 1U;
  WriteTile4bpp(snes, 0x1000U, 0U, solid_1, now);
  WriteTile4bpp(snes, 0x4000U, 1U, solid_1, now);

  // BG1 tilemap (0,0): char 0, priority 1.
  WriteVramWord(snes, 0x0000U, 0x2000U, now);

  BusWrite(snes, sppu::regs::kObsel, 0x02U, now++);
  // OBJ 0: priority 0 (attr bits 5:4 = 0).
  WriteOamLowEntry(snes, 0U, 20U, 30U, 1U, 0x00U, now);

  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);
  BusWrite(snes, sppu::regs::kBg1Sc, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg12Nba, 0x01U, now++);
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Hofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBg1Vofs, 0x00U, now++);
  BusWrite(snes, sppu::regs::kTm, static_cast<uint8_t>(sppu::regs::kTmBg1Mask | sppu::regs::kTmObjMask), now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();

  // BG1 prio-1 sits above OBJ.0 → red wins where they overlap.
  REQUIRE(view.pixels[30U * view.stride + 20U] == 0x001FU);
}

TEST_CASE("OBJ overlap: lowest OAM index wins among covering sprites", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);
  WriteCgramWord(snes, 0x81U, 0x001FU, now);  // OBJ 0 → red
  WriteCgramWord(snes, 0x91U, 0x03E0U, now);  // OBJ 1 → green (palette group 1, color 1 → 0x80 + 16 + 1 = 0x91)

  uint8_t solid_1[64];
  for (uint16_t i = 0; i < 64; ++i) solid_1[i] = 1U;
  WriteTile4bpp(snes, 0x0000U, 1U, solid_1, now);

  BusWrite(snes, sppu::regs::kObsel, 0x00U, now++);
  // OBJ 0 (lower index, should win): position (40,40), palette 0.
  WriteOamLowEntry(snes, 0U, 40U, 40U, 1U, 0x00U, now);
  // OBJ 1 (higher index, same position): palette 1 (attr bits 3:1 = 0b001 → 0x02).
  WriteOamLowEntry(snes, 1U, 40U, 40U, 1U, 0x02U, now);

  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);
  BusWrite(snes, sppu::regs::kTm, sppu::regs::kTmObjMask, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();

  // OBJ 0 (red) wins by virtue of lower OAM index.
  REQUIRE(view.pixels[40U * view.stride + 40U] == 0x001FU);
}

TEST_CASE("OBJ TM bit 4 disables sprites globally", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x7FFFU, now);     // backdrop = white
  WriteCgramWord(snes, 0x81U, 0x001FU, now);  // would-be OBJ red

  uint8_t solid_1[64];
  for (uint16_t i = 0; i < 64; ++i) solid_1[i] = 1U;
  WriteTile4bpp(snes, 0x0000U, 1U, solid_1, now);

  BusWrite(snes, sppu::regs::kObsel, 0x00U, now++);
  WriteOamLowEntry(snes, 0U, 60U, 60U, 1U, 0x00U, now);

  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);
  // TM = 0 → no main-screen layers, including OBJ.
  BusWrite(snes, sppu::regs::kTm, 0x00U, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();
  // Sprite suppressed → backdrop everywhere.
  REQUIRE(view.pixels[60U * view.stride + 60U] == 0x7FFFU);
}

TEST_CASE("OBJ color index 0 is transparent and falls through to backdrop", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x7C1FU, now);     // backdrop magenta
  WriteCgramWord(snes, 0x81U, 0x001FU, now);  // OBJ color 1 (unused)

  // OBJ tile 1 entirely color 0 (transparent for sprites too).
  uint8_t empty[64] = {};
  WriteTile4bpp(snes, 0x0000U, 1U, empty, now);

  BusWrite(snes, sppu::regs::kObsel, 0x00U, now++);
  WriteOamLowEntry(snes, 0U, 70U, 70U, 1U, 0x00U, now);

  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);
  BusWrite(snes, sppu::regs::kTm, sppu::regs::kTmObjMask, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();
  REQUIRE(view.pixels[70U * view.stride + 70U] == 0x7C1FU);
}

// ---------------------------------------------------------------------------
// Color-math tests.
// ---------------------------------------------------------------------------
// Anchor: SMW title screen produces a black sky without color math because the
// PPU never adds the COLDATA fixed colour to the backdrop. These tests cover
// each leg of the math path (register decoding, layer participation gating,
// add/subtract/halve, saturation, sub-screen fallback to COLDATA).

TEST_CASE("COLDATA writes accumulate R/G/B latches independently", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  // Each write specifies which channel(s) to update via bits 5/6/7. Channels
  // not selected by a write keep their prior value, so three single-channel
  // writes assemble a 15-bit BGR triple.
  BusWrite(snes, sppu::regs::kColdata, 0x25U, now++);  // R = 5
  BusWrite(snes, sppu::regs::kColdata, 0x4AU, now++);  // G = 10
  BusWrite(snes, sppu::regs::kColdata, 0x8FU, now++);  // B = 15
  // Drain the lazy-replay log via a read.
  (void)BusRead(snes, sppu::regs::kStat77, now++);

  REQUIRE(ppu.GetColdataR() == 5);
  REQUIRE(ppu.GetColdataG() == 10);
  REQUIRE(ppu.GetColdataB() == 15);

  // A multi-channel write overwrites all selected channels with the same
  // intensity; unselected channels persist.
  BusWrite(snes, sppu::regs::kColdata, 0xE0U | 7U, now++);  // R+G+B = 7
  (void)BusRead(snes, sppu::regs::kStat77, now++);
  REQUIRE(ppu.GetColdataR() == 7);
  REQUIRE(ppu.GetColdataG() == 7);
  REQUIRE(ppu.GetColdataB() == 7);
}

TEST_CASE("Backdrop + fixed COLDATA: math adds COLDATA to backdrop", "[unit][ppu]") {
  // Mode 1, all BG layers disabled and OBJ disabled, so every visible pixel
  // resolves to the backdrop (CGRAM[0]). With CGADSUB.5 set and CGWSEL.1
  // clear, the sub-screen source is fixed COLDATA — final = CGRAM[0] +
  // COLDATA per channel.
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);  // brightness=15
  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);   // Mode 1
  // CGRAM[0] = dark blue R=2 G=2 B=5.
  WriteCgramWord(snes, 0U, static_cast<uint16_t>(2U | (2U << 5U) | (5U << 10U)), now);
  // COLDATA fixed = R=10 G=10 B=10.
  BusWrite(snes, sppu::regs::kColdata, 0x20U | 10U, now++);
  BusWrite(snes, sppu::regs::kColdata, 0x40U | 10U, now++);
  BusWrite(snes, sppu::regs::kColdata, 0x80U | 10U, now++);
  // Enable color math for backdrop, source = fixed (CGWSEL.1 clear), add.
  BusWrite(snes, sppu::regs::kCgwsel, 0x00U, now++);
  BusWrite(snes, sppu::regs::kCgadsub, sppu::regs::kCgadsubBackdropMask, now++);
  // TM = 0 (nothing renders) — entire visible area resolves to backdrop.
  BusWrite(snes, sppu::regs::kTm, 0x00U, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();
  // Expected: R = 2+10 = 12, G = 2+10 = 12, B = 5+10 = 15.
  const uint16_t expected = static_cast<uint16_t>(12U | (12U << 5U) | (15U << 10U));
  REQUIRE(view.pixels[100U * view.stride + 100U] == expected);
}

TEST_CASE("Color math add saturates per channel at 31", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);
  // Backdrop = R=25 G=0 B=0; COLDATA = R=10 G=0 B=0 → 25+10 should clip to 31.
  WriteCgramWord(snes, 0U, static_cast<uint16_t>(25U), now);
  BusWrite(snes, sppu::regs::kColdata, 0x20U | 10U, now++);
  BusWrite(snes, sppu::regs::kCgwsel, 0x00U, now++);
  BusWrite(snes, sppu::regs::kCgadsub, sppu::regs::kCgadsubBackdropMask, now++);
  BusWrite(snes, sppu::regs::kTm, 0x00U, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();
  REQUIRE((view.pixels[100U * view.stride + 100U] & 0x1FU) == 31U);
}

TEST_CASE("Color math subtract saturates per channel at 0", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);
  WriteCgramWord(snes, 0U, static_cast<uint16_t>(5U | (5U << 5U) | (5U << 10U)), now);
  // COLDATA = R=10 G=10 B=10. Subtract → all channels clip to 0.
  BusWrite(snes, sppu::regs::kColdata, 0x20U | 10U, now++);
  BusWrite(snes, sppu::regs::kColdata, 0x40U | 10U, now++);
  BusWrite(snes, sppu::regs::kColdata, 0x80U | 10U, now++);
  BusWrite(snes, sppu::regs::kCgwsel, 0x00U, now++);
  BusWrite(snes, sppu::regs::kCgadsub,
           static_cast<uint8_t>(sppu::regs::kCgadsubBackdropMask | sppu::regs::kCgadsubSubtractMask), now++);
  BusWrite(snes, sppu::regs::kTm, 0x00U, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();
  REQUIRE(view.pixels[100U * view.stride + 100U] == 0U);
}

TEST_CASE("Color math half divides each channel of the final by two", "[unit][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);
  WriteCgramWord(snes, 0U, static_cast<uint16_t>(10U | (10U << 5U) | (10U << 10U)), now);
  BusWrite(snes, sppu::regs::kColdata, 0x20U | 10U, now++);
  BusWrite(snes, sppu::regs::kColdata, 0x40U | 10U, now++);
  BusWrite(snes, sppu::regs::kColdata, 0x80U | 10U, now++);
  BusWrite(snes, sppu::regs::kCgwsel, 0x00U, now++);
  // Add + halve: result per channel = (10+10) / 2 = 10.
  BusWrite(snes, sppu::regs::kCgadsub,
           static_cast<uint8_t>(sppu::regs::kCgadsubBackdropMask | sppu::regs::kCgadsubHalfMask), now++);
  BusWrite(snes, sppu::regs::kTm, 0x00U, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();
  const uint16_t expected = static_cast<uint16_t>(10U | (10U << 5U) | (10U << 10U));
  REQUIRE(view.pixels[100U * view.stride + 100U] == expected);
}

TEST_CASE("Sub-screen BG2 pixel is the math source when CGWSEL.1 is set", "[unit][ppu]") {
  // Full path: backdrop math-enabled, sub-screen BG2 enabled, CGWSEL.1 set so
  // BG2 pixels (where rendered) feed the sub-screen instead of COLDATA.
  // Replicates SMW title's "BG2 cloud silhouettes visible inside the
  // backdrop region".
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  // CGRAM[0] = black backdrop. CGRAM[1] = bright green (BG2 palette 0 colour 1).
  WriteCgramWord(snes, 0U, 0x0000U, now);
  WriteCgramWord(snes, 1U, static_cast<uint16_t>(0U | (31U << 5U) | 0U), now);

  // BG2 tile 0: leftmost pixel of row 0 is palette index 1, rest transparent.
  uint8_t tile[64] = {};
  tile[0] = 1U;
  WriteTile4bpp(snes, /*char_word_base=*/0x2000U, /*char_index=*/0, tile, now);
  // BG2 tilemap entry (0,0) → char 0, palette 0, no flip.
  WriteVramWord(snes, /*word_addr=*/0x1000U, /*value=*/0x0000U, now);
  // BG2SC: tilemap base $1000 words = data byte $10 << 2 = $40; layout 32x32.
  BusWrite(snes, sppu::regs::kBg2Sc, 0x40U, now++);
  // BG12NBA high nibble = BG2 char base = 2 → $2000 words.
  BusWrite(snes, sppu::regs::kBg12Nba, 0x20U, now++);
  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);

  // TM = 0 (everything resolves to backdrop on main).
  BusWrite(snes, sppu::regs::kTm, 0x00U, now++);
  // TS = BG2 only — BG2 renders on sub-screen.
  BusWrite(snes, sppu::regs::kTs, sppu::regs::kTmBg2Mask, now++);
  // CGWSEL.1 set: sub-screen BG/OBJ participate. COLDATA = 0 so backdrop +
  // BG2 pixel = BG2 pixel.
  BusWrite(snes, sppu::regs::kCgwsel, sppu::regs::kCgwselSubScreenEnableMask, now++);
  BusWrite(snes, sppu::regs::kCgadsub, sppu::regs::kCgadsubBackdropMask, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();
  // Pixel (0, 0) should show BG2's green; pixel (4, 0) is still BG2-transparent
  // → falls back to COLDATA (0) → backdrop + 0 = backdrop = black.
  const uint16_t green = static_cast<uint16_t>(0U | (31U << 5U) | 0U);
  REQUIRE(view.pixels[0U * view.stride + 0U] == green);
  REQUIRE(view.pixels[0U * view.stride + 4U] == 0U);
}

TEST_CASE("CGADSUB layer mask gates math per-layer", "[unit][ppu]") {
  // Confirm a foreground layer (BG1) that ISN'T in CGADSUB stays untouched
  // while backdrop pixels still get math applied. Strategy: BG1 tile 0 has
  // exactly one opaque pixel (top-left of the 8×8); the rest is transparent
  // and falls through to the backdrop. With the BG1 tilemap left zeroed,
  // every BG cell points to char 0 — so the visible result is a tight grid
  // of opaque dots over a backdrop field.
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);  // backdrop = black
  WriteCgramWord(snes, 1U, 0x7C00U, now);  // BG1 palette 0 colour 1 = blue
  // Tile 0: only pixel (0,0) is palette index 1; all others are 0 (transparent).
  uint8_t tile[64] = {};
  tile[0] = 1U;
  WriteTile4bpp(snes, /*char_word_base=*/0x1000U, /*char_index=*/0, tile, now);
  BusWrite(snes, sppu::regs::kBg12Nba, 0x01U, now++);  // low nibble = BG1 base = 1 ($1000 words)
  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);
  BusWrite(snes, sppu::regs::kTm, sppu::regs::kTmBg1Mask, now++);

  // COLDATA = R=15 only; CGWSEL.1 = 0 so sub source = fixed.
  BusWrite(snes, sppu::regs::kColdata, 0x20U | 15U, now++);
  BusWrite(snes, sppu::regs::kCgwsel, 0x00U, now++);
  // CGADSUB: backdrop only (BG1 NOT included).
  BusWrite(snes, sppu::regs::kCgadsub, sppu::regs::kCgadsubBackdropMask, now++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();
  // Pixel (0,0): BG1 opaque dot — math doesn't apply because BG1 isn't in
  // CGADSUB, so we see the raw palette colour.
  REQUIRE(view.pixels[0U * view.stride + 0U] == 0x7C00U);
  // Pixel (1,0): BG1 transparent at this tile-local position → backdrop.
  // Backdrop is in CGADSUB and gets `0 + 15` on R = 15.
  REQUIRE(view.pixels[0U * view.stride + 1U] == 15U);
}

// ---------------------------------------------------------------------------
// SMW end-to-end visual check ([smwbug]). Boots the ROM and verifies that the
// "far background" in the middle of the title screen is no longer the black
// backdrop — it should resolve through color math to a non-zero blue derived
// from COLDATA (and BG2 cloud silhouettes where they paint on the sub-screen).
// Tagged [smwbug] so the diagnostic dump can also be invoked in isolation.
// ---------------------------------------------------------------------------

#include <filesystem>
#include <fstream>
#include <iterator>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/rom/rom_format.h"

namespace {
std::vector<uint8_t> ReadRomFile(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}  // namespace

TEST_CASE("SMW title-screen color-math fills the sky region", "[smwbug]") {
  using namespace pupsnes;
  std::filesystem::path rom_path = "roms/Super Mario World (U) [!].smc";
  auto rom = ReadRomFile(rom_path);
  if (rom.empty()) {
    // Commercial ROM not present in this checkout — skip rather than fail so
    // contributors without the file can still pull and build cleanly. The
    // unit tests above cover the color-math logic itself.
    SKIP("Super Mario World ROM not present at " << rom_path);
  }
  StripSmcCopierHeader(rom);

  SNES snes;
  snes.LoadRom(rom);
  snes.Reset();

  const uint32_t kFrames = 600;
  const TimeMasterT cycles_per_frame = 262U * 1364U;
  const TimeMasterT target = static_cast<TimeMasterT>(kFrames) * cycles_per_frame;
  while (snes.GetMasterTime() < target) {
    TimeMasterT next_event = snes.GetScheduler().NextEventMasterTime();
    TimeMasterT tick_target = next_event > target ? target : next_event;
    (void)snes.GetCpu().TickToTarget(tick_target);
    TimeMasterT after = snes.GetMasterTime();
    snes.MachineSync(after);
    snes.GetScheduler().FireEventsThrough(after);
  }

  Ppu& ppu = snes.GetPpu();
  ppu.CatchUpTo(snes.GetMasterTime());

  // Dump a PPM snapshot for visual inspection of the rendered title screen.
  FrameBufferView view = ppu.BuildFrontView();
  {
    std::ofstream ppm("/tmp/smw_dump.ppm", std::ios::binary);
    ppm << "P6\n" << view.width << " " << view.height << "\n255\n";
    for (uint32_t y = 0; y < view.height; ++y) {
      for (uint32_t x = 0; x < view.width; ++x) {
        const uint16_t px = view.pixels[y * view.stride + x];
        ppm.put(static_cast<char>((px & 0x1FU) << 3));
        ppm.put(static_cast<char>(((px >> 5) & 0x1FU) << 3));
        ppm.put(static_cast<char>(((px >> 10) & 0x1FU) << 3));
      }
    }
  }

  // The pre-fix bug was: middle rows (where SMW's "far background" lives)
  // dropped to pure $0000 black because the backdrop never went through
  // color math. With math wired up, row 64 column 128 must no longer be
  // raw black — it should pick up COLDATA's accumulated sky-blue.
  const uint16_t mid_pixel = view.pixels[64U * view.stride + 128U];
  REQUIRE(mid_pixel != 0U);

  // Sanity: the same row should have at least one non-backdrop pixel from
  // BG2 sub-screen compositing if the cloud silhouettes hit there.
  uint32_t distinct = 0;
  uint16_t first = view.pixels[64U * view.stride + 0U];
  for (uint32_t x = 0; x < view.width; ++x) {
    if (view.pixels[64U * view.stride + x] != first) {
      ++distinct;
      break;
    }
  }
  REQUIRE(distinct > 0U);
}

TEST_CASE("Mid-line VRAM write to BG1 tile data propagates to subsequent pixels", "[unit][ppu]") {
  // Regression for the BG row cache: even when consecutive pixels stay within
  // the same tile (so tile_x doesn't change), a VMDATA write that lands
  // mid-line must invalidate the cached plane bytes — otherwise the cached
  // copy keeps painting the pre-write color.
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);  // backdrop = black
  WriteCgramWord(snes, 1U, 0x001FU, now);  // BG1 palette idx 1 = red
  WriteCgramWord(snes, 3U, 0x7C00U, now);  // BG1 palette idx 3 = blue

  // Tile 0 starts as solid color-index 1 (red): plane 0 = 0xFF, planes 1/2/3
  // = 0 on every row. The mid-line write below flips plane 1, row 0 to 0xFF,
  // promoting that row's pixels to color-index 3 (blue).
  uint8_t tile_red[64];
  for (uint16_t i = 0; i < 64; ++i) tile_red[i] = 1U;
  WriteTile4bpp(snes, 0x1000U, 0U, tile_red, now);

  // Tilemap entry 0 at word base 0: tile index 0, palette group 0, no flip.
  WriteVramWord(snes, 0x0000U, 0x0000U, now);
  // Park VMADDR at word 0x1000 so the mid-line VMDATAH write targets byte
  // 0x2001 (plane-1, row-0) directly — no need to re-set the address mid-line.
  SetVramAddress(snes, 0x1000U, now);

  BusWrite(snes, sppu::regs::kBg12Nba, 0x01U, now++);
  BusWrite(snes, sppu::regs::kBg1Sc, 0x00U, now++);
  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);
  BusWrite(snes, sppu::regs::kTm, sppu::regs::kTmBg1Mask, now++);

  // Render through screen_x=5 of line V=1. Dot H=27 ends at master cycle
  // 1*1364 + (27 - 0 + 1)*4 = 1476 (each pre-long dot costs 4 mcyc).
  ppu.CatchUpTo(1476U);

  // Mid-line: one VMDATAH write. With VMAIN.7=1 it lands on byte 0x2001 of
  // VRAM = plane-1 of tile-0, row-0. Pixel color becomes p0=1, p1=1 → idx 3.
  TimeMasterT mid = 1477U;
  BusWrite(snes, sppu::regs::kVmDataH, 0xFFU, mid++);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();

  // Pixels rendered before the VRAM write keep the red color.
  REQUIRE(view.pixels[0U * view.stride + 0U] == 0x001FU);
  REQUIRE(view.pixels[0U * view.stride + 5U] == 0x001FU);
  // Pixel rendered after the VRAM write reads the new plane bytes → blue.
  // (Same tile_x as screen_x=5 — both inside tile 0 — so this only passes
  // when the cache notices the VMDATA invalidation.)
  REQUIRE(view.pixels[0U * view.stride + 7U] == 0x7C00U);
}

TEST_CASE("OBJ list latched per scanline — mid-line OAM write does not unrender sprite", "[unit][ppu]") {
  // Per-scanline OAM evaluation invariant: once a line's sprite list is
  // latched at line start, an OAM write that lands mid-line must NOT alter
  // the sprite drawn on that line. On hardware the evaluation pass runs in
  // the tail of the previous scanline, so by the time visible pixels start
  // emitting the list is fixed.
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  ppu.Reset();
  TimeMasterT now = 1;

  BusWrite(snes, sppu::regs::kInidisp, 0x0FU, now++);
  BusWrite(snes, sppu::regs::kVmain, 0x80U, now++);
  WriteCgramWord(snes, 0U, 0x0000U, now);     // backdrop = black
  WriteCgramWord(snes, 0x81U, 0x001FU, now);  // OBJ palette 0 color 1 = red

  // Solid red OBJ tile at char index 1 in region 0.
  uint8_t solid_red[64];
  for (uint16_t i = 0; i < 64; ++i) solid_red[i] = 1U;
  WriteTile4bpp(snes, 0x0000U, 1U, solid_red, now);

  BusWrite(snes, sppu::regs::kObsel, 0x00U, now++);
  // OBJ 0: 8x8 sprite at screen (10, 20). H range of sprite = 32..39 on
  // dot grid; V = 21 (screen_y=20 → V=1+20=21).
  WriteOamLowEntry(snes, 0U, 10U, 20U, 1U, 0x00U, now);

  BusWrite(snes, sppu::regs::kBgmode, 0x01U, now++);
  BusWrite(snes, sppu::regs::kTm, sppu::regs::kTmObjMask, now++);

  // Render through screen_x=12 of V=21. Line 21 starts at cycle 21*1364 =
  // 28644; H=22 begins visible at 28644+22*4=28732; H=34 (screen_x=12) ends
  // at 28644+(34-22+1)*4+22*4 — easier: each dot is 4 mcyc and there are 13
  // dots from H=22..34 inclusive emitted, so target = 28732 + 13*4 = 28784.
  ppu.CatchUpTo(28784U);

  // Mid-line OAM write to move OBJ 0 to Y=100 (off the current line). All
  // other fields kept the same. The Y byte (OAM offset 1) commits at the
  // second OAMDATA write inside WriteOamLowEntry.
  TimeMasterT mid = 28786U;
  WriteOamLowEntry(snes, 0U, 10U, 100U, 1U, 0x00U, mid);

  ppu.CatchUpTo(kFrameEndNtsc);
  const FrameBufferView view = ppu.BuildFrontView();

  // Sprite columns 10..12 emitted before the OAM write — red on any model.
  REQUIRE(view.pixels[20U * view.stride + 10U] == 0x001FU);
  REQUIRE(view.pixels[20U * view.stride + 12U] == 0x001FU);
  // Sprite columns 15..17 emitted after the OAM write. With the per-line
  // latch they STAY red because the list was already evaluated for line 21.
  // (Without the latch the per-pixel scan would see Y=100, drop the sprite,
  // and these pixels would fall through to the black backdrop.)
  REQUIRE(view.pixels[20U * view.stride + 15U] == 0x001FU);
  REQUIRE(view.pixels[20U * view.stride + 17U] == 0x001FU);
}
