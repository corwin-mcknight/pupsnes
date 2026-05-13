#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "pupsnes/hw/joypad.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)

namespace {

uint8_t BusRead(SNES& snes, uint32_t addr) {
  BusPlan plan = snes.system_bus->Plan(addr, BusAccessType::kRead);
  return snes.system_bus->Follow(plan, /*current_time=*/0, 0).data;
}

void BusWrite(SNES& snes, uint32_t addr, uint8_t data) {
  BusPlan plan = snes.system_bus->Plan(addr, BusAccessType::kWrite, data);
  (void)snes.system_bus->Follow(plan, /*current_time=*/0, 0);
}

}  // namespace

TEST_CASE("Joypad::SetButton sets the matching bit in the 16-bit state", "[unit][joypad]") {
  SNES snes;
  Joypad& pad = snes.GetJoypad();

  pad.SetButton(Joypad::Button::kB, true);
  pad.SetButton(Joypad::Button::kStart, true);
  pad.SetButton(Joypad::Button::kRight, true);
  pad.SetButton(Joypad::Button::kA, true);
  pad.SetButton(Joypad::Button::kR, true);

  // Layout: bit 15 B, 12 Start, 8 Right, 7 A, 4 R -> mask 0x9190
  REQUIRE(pad.GetP1State() == 0x9190U);
  REQUIRE(pad.GetButton(Joypad::Button::kB));
  REQUIRE(pad.GetButton(Joypad::Button::kStart));
  REQUIRE_FALSE(pad.GetButton(Joypad::Button::kY));

  pad.SetButton(Joypad::Button::kB, false);
  REQUIRE_FALSE(pad.GetButton(Joypad::Button::kB));
  REQUIRE(pad.GetP1State() == 0x1190U);

  pad.ReleaseAll();
  REQUIRE(pad.GetP1State() == 0x0000U);
}

TEST_CASE("$4218/$4219 read the current P1 button bitmask", "[unit][joypad]") {
  SNES snes;
  snes.Reset();
  Joypad& pad = snes.GetJoypad();

  // Press B + Up + A. Bits set: 15, 11, 7 -> 0x8880
  pad.SetButton(Joypad::Button::kB, true);
  pad.SetButton(Joypad::Button::kUp, true);
  pad.SetButton(Joypad::Button::kA, true);

  REQUIRE(BusRead(snes, 0x00'4218U) == 0x80U);  // low byte: bit 7 (A)
  REQUIRE(BusRead(snes, 0x00'4219U) == 0x88U);  // high byte: bits 15 (B), 11 (Up)

  // Other JOY result registers (P2/P3/P4) stay zero.
  for (uint32_t addr = 0x421AU; addr <= 0x421FU; ++addr) {
    REQUIRE(BusRead(snes, addr) == 0x00U);
  }
}

TEST_CASE("$4017 always reads 0 on the data line (no P2 controller)", "[unit][joypad]") {
  SNES snes;
  snes.Reset();
  // Even with P1 fully pressed, $4017 carries P2's data line which is unwired.
  snes.GetJoypad().SetButton(Joypad::Button::kB, true);
  REQUIRE((BusRead(snes, 0x00'4017U) & 0x01U) == 0x00U);
}

TEST_CASE("$4016 manual serial: strobe + 16 reads clock out P1 state MSB-first", "[unit][joypad]") {
  SNES snes;
  snes.Reset();
  Joypad& pad = snes.GetJoypad();

  // Distinct pattern: B (15) + Y (14) + Start (12) + Right (8) + A (7) + R (4)
  // = 0xD190. The expected serial output (MSB first) is the 16-bit value above,
  // then ones forever.
  pad.SetButton(Joypad::Button::kB, true);
  pad.SetButton(Joypad::Button::kY, true);
  pad.SetButton(Joypad::Button::kStart, true);
  pad.SetButton(Joypad::Button::kRight, true);
  pad.SetButton(Joypad::Button::kA, true);
  pad.SetButton(Joypad::Button::kR, true);
  REQUIRE(pad.GetP1State() == 0xD190U);

  // Standard latch sequence: write 1, then 0 to $4016.
  BusWrite(snes, 0x00'4016U, 0x01U);
  BusWrite(snes, 0x00'4016U, 0x00U);

  constexpr uint16_t kExpected = 0xD190U;
  for (int i = 15; i >= 0; --i) {
    const uint8_t expected_bit = static_cast<uint8_t>((kExpected >> i) & 0x01U);
    const uint8_t got = BusRead(snes, 0x00'4016U) & 0x01U;
    REQUIRE(got == expected_bit);
  }
  // Past the 16-bit window, an SNES standard controller's data line floats
  // high -> every further read returns 1.
  for (int i = 0; i < 4; ++i) {
    REQUIRE((BusRead(snes, 0x00'4016U) & 0x01U) == 0x01U);
  }
}

TEST_CASE("$4016 strobe held high returns the live B-button bit", "[unit][joypad]") {
  SNES snes;
  snes.Reset();
  Joypad& pad = snes.GetJoypad();

  // Strobe high — pad data continuously latches. Reads sample B (bit 15).
  BusWrite(snes, 0x00'4016U, 0x01U);

  pad.SetButton(Joypad::Button::kB, false);
  REQUIRE((BusRead(snes, 0x00'4016U) & 0x01U) == 0x00U);

  pad.SetButton(Joypad::Button::kB, true);
  REQUIRE((BusRead(snes, 0x00'4016U) & 0x01U) == 0x01U);
}

TEST_CASE("Joypad::Reset clears the manual shift state but not button presses", "[unit][joypad]") {
  SNES snes;
  Joypad& pad = snes.GetJoypad();
  pad.SetButton(Joypad::Button::kB, true);

  // Start a manual read mid-sequence.
  BusWrite(snes, 0x00'4016U, 0x01U);
  BusWrite(snes, 0x00'4016U, 0x00U);
  (void)BusRead(snes, 0x00'4016U);  // consume bit 0 of shift register

  snes.Reset();

  // Button state is owned by the UI/user and survives Reset.
  REQUIRE(pad.GetButton(Joypad::Button::kB));

  // A fresh latch sequence after reset clocks out bit 15 of the live state
  // (B = 1), proving the strobe edge re-snapshots p1_state_.
  BusWrite(snes, 0x00'4016U, 0x01U);
  BusWrite(snes, 0x00'4016U, 0x00U);
  REQUIRE((BusRead(snes, 0x00'4016U) & 0x01U) == 0x01U);
}

TEST_CASE("Manual joypad ports $4016/$4017 cost 12 master cycles per access", "[unit][joypad]") {
  // Pages $40-$41 ($4000-$41FF) are the slowest bus class on real hardware —
  // every access takes 12 master cycles, FASTROM has no effect. Mirror this in
  // both bank $00 (low-half) and bank $80 (high-half) since the MMIO span is
  // mirrored there too.
  SNES snes;
  snes.Reset();

  for (uint32_t addr : {0x00'4016U, 0x00'4017U, 0x80'4016U, 0x80'4017U}) {
    const BusPlan read_plan = snes.system_bus->Plan(addr, BusAccessType::kRead);
    REQUIRE(read_plan.outcome == BusPlanOutcome::kInlineComplete);
    REQUIRE(read_plan.access_cycles == 12);

    const BusPlan write_plan = snes.system_bus->Plan(addr, BusAccessType::kWrite, 0x00U);
    REQUIRE(write_plan.outcome == BusPlanOutcome::kInlineComplete);
    REQUIRE(write_plan.access_cycles == 12);
  }
}

TEST_CASE("Unmapped $4000-$41FF addresses still bill 12 master cycles", "[unit][joypad]") {
  // The whole manual-joypad page range bills at 12 mcyc on real hardware, even
  // for addresses with no live device behind them ($4000-$4015, $4018-$41FF).
  // PupSNES routes every page in this span through CpuMmio as kSameClockMmio
  // so the bus log captures the access; the access_cycles must still be 12.
  SNES snes;
  snes.Reset();

  for (uint32_t addr : {0x00'4000U, 0x00'4018U, 0x00'40FFU, 0x00'4100U, 0x00'41FFU}) {
    const BusPlan plan = snes.system_bus->Plan(addr, BusAccessType::kRead);
    REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
    REQUIRE(plan.access_cycles == 12);
  }
}

TEST_CASE("Auto-joypad result regs $4218-$421F use the 8-cycle MMIO timing", "[unit][joypad]") {
  // $4218-$421F live on page $42, which is part of the standard CPU MMIO
  // block — 8 master cycles in PupSNES today. (Real hardware bills 6 for
  // $4200-$43FF; see TODO.md for the wider fix.) This test pins the current
  // contract so a future bump to 6 trips a deliberate update here.
  SNES snes;
  snes.Reset();

  for (uint32_t addr = 0x00'4218U; addr <= 0x00'421FU; ++addr) {
    const BusPlan plan = snes.system_bus->Plan(addr, BusAccessType::kRead);
    REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
    REQUIRE(plan.access_cycles == 8);
  }
}
