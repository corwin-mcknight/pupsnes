#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "pupsnes/core/scheduler.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/hw/input/joypad.h"
#include "pupsnes/memory/systembus.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)

namespace {

uint8_t BusRead(SNES& snes, uint32_t addr, TimeMasterT now = 0) {
  BusPlan plan = snes.system_bus->Plan(addr, BusAccessType::kRead);
  return snes.system_bus->Follow(plan, now, 0).data;
}

void BusWrite(SNES& snes, uint32_t addr, uint8_t data, TimeMasterT now = 0) {
  BusPlan plan = snes.system_bus->Plan(addr, BusAccessType::kWrite, data);
  (void)snes.system_bus->Follow(plan, now, 0);
}

void RunToTime(SNES& snes, TimeMasterT target) {
  while (snes.GetScheduler().NextEventMasterTime() <= target) {
    const TimeMasterT next = snes.GetScheduler().NextEventMasterTime();
    snes.MachineSync(next);
    snes.GetScheduler().FireEventsThrough(next);
  }
  snes.MachineSync(target);
}

constexpr TimeMasterT kVblankStart = 225U * 1364U;
constexpr TimeMasterT kFirstPollStart = kVblankStart + 298U;
constexpr TimeMasterT kFirstPollEnd = kFirstPollStart + 4224U;

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

TEST_CASE("$4218/$4219 latch P1 at the end of auto polling", "[unit][joypad]") {
  SNES snes;
  snes.Reset();
  Joypad& pad = snes.GetJoypad();

  // Press B + Up + A. Bits set: 15, 11, 7 -> 0x8880
  pad.SetButton(Joypad::Button::kB, true);
  pad.SetButton(Joypad::Button::kUp, true);
  pad.SetButton(Joypad::Button::kA, true);

  REQUIRE(BusRead(snes, 0x00'4218U) == 0x00U);
  BusWrite(snes, 0x00'4200U, 0x01U);
  RunToTime(snes, kFirstPollEnd);
  REQUIRE(BusRead(snes, 0x00'4218U, kFirstPollEnd) == 0x80U);  // low byte: bit 7 (A)
  REQUIRE(BusRead(snes, 0x00'4219U, kFirstPollEnd) == 0x88U);  // high byte: bits 15 (B), 11 (Up)

  // Other JOY result registers stay zero when no other buttons are held.
  for (uint32_t addr = 0x421AU; addr <= 0x421FU; ++addr) {
    REQUIRE(BusRead(snes, addr, kFirstPollEnd) == 0x00U);
  }
}

TEST_CASE("P2 auto-read and manual serial state are independent of P1", "[unit][joypad]") {
  SNES snes;
  snes.Reset();
  Joypad& pad = snes.GetJoypad();
  pad.SetButton(Joypad::Button::kB, true);
  pad.SetButton(Joypad::Button::kY, true, 1);
  pad.SetButton(Joypad::Button::kA, true, 1);
  REQUIRE(pad.GetP1State() == 0x8000U);
  REQUIRE(pad.GetP2State() == 0x4080U);
  BusWrite(snes, 0x00'4200U, 0x01U);
  RunToTime(snes, kFirstPollEnd);
  REQUIRE(BusRead(snes, 0x00'4218U, kFirstPollEnd) == 0x00U);
  REQUIRE(BusRead(snes, 0x00'4219U, kFirstPollEnd) == 0x80U);
  REQUIRE(BusRead(snes, 0x00'421AU, kFirstPollEnd) == 0x80U);
  REQUIRE(BusRead(snes, 0x00'421BU, kFirstPollEnd) == 0x40U);

  BusWrite(snes, 0x00'4016U, 1, kFirstPollEnd);
  BusWrite(snes, 0x00'4016U, 0, kFirstPollEnd);
  REQUIRE((BusRead(snes, 0x00'4016U, kFirstPollEnd) & 1U) == 1U);
  REQUIRE((BusRead(snes, 0x00'4017U, kFirstPollEnd) & 1U) == 0U);
  REQUIRE((BusRead(snes, 0x00'4017U, kFirstPollEnd) & 1U) == 1U);
  pad.SetButton(Joypad::Button::kB, true, 1);
  REQUIRE((BusRead(snes, 0x00'4017U, kFirstPollEnd) & 1U) == 0U);
  BusWrite(snes, 0x00'4016U, 1, kFirstPollEnd);
  REQUIRE((BusRead(snes, 0x00'4017U, kFirstPollEnd) & 1U) == 1U);
  BusWrite(snes, 0x00'4016U, 0, kFirstPollEnd);
  for (int i = 0; i < 16; ++i) (void)BusRead(snes, 0x00'4017U, kFirstPollEnd);
  REQUIRE((BusRead(snes, 0x00'4017U, kFirstPollEnd) & 1U) == 1U);

  pad.ReleaseAll(1);
  REQUIRE(pad.GetP2State() == 0);
  REQUIRE(pad.GetP1State() == 0x8000U);
}

TEST_CASE("Automatic poll exposes a delayed busy window and incremental results", "[unit][joypad]") {
  SNES snes;
  snes.Reset();
  auto& pad = snes.GetJoypad();
  pad.SetButton(Joypad::Button::kB, true);
  pad.SetButton(Joypad::Button::kA, true);
  pad.SetButton(Joypad::Button::kY, true, 1);
  BusWrite(snes, 0x4200U, 1U);

  RunToTime(snes, kFirstPollStart - 1U);
  REQUIRE((BusRead(snes, 0x4212U, kFirstPollStart - 1U) & 1U) == 0U);
  REQUIRE(BusRead(snes, 0x4219U, kFirstPollStart - 1U) == 0U);

  RunToTime(snes, kFirstPollStart);
  REQUIRE((BusRead(snes, 0x4212U, kFirstPollStart) & 1U) == 1U);
  REQUIRE(BusRead(snes, 0x4219U, kFirstPollStart) == 0U);

  RunToTime(snes, kFirstPollStart + 383U);
  REQUIRE(BusRead(snes, 0x4219U, kFirstPollStart + 383U) == 0U);
  RunToTime(snes, kFirstPollStart + 384U);
  REQUIRE(BusRead(snes, 0x4218U, kFirstPollStart + 384U) == 1U);
  REQUIRE(BusRead(snes, 0x421AU, kFirstPollStart + 384U) == 0U);

  pad.SetButton(Joypad::Button::kB, false);
  pad.SetButton(Joypad::Button::kRight, true, 1);
  RunToTime(snes, kFirstPollEnd - 1U);
  REQUIRE((BusRead(snes, 0x4212U, kFirstPollEnd - 1U) & 1U) == 1U);
  RunToTime(snes, kFirstPollEnd);
  REQUIRE((BusRead(snes, 0x4212U, kFirstPollEnd) & 1U) == 0U);
  REQUIRE(BusRead(snes, 0x4218U, kFirstPollEnd) == 0x80U);
  REQUIRE(BusRead(snes, 0x4219U, kFirstPollEnd) == 0x80U);
  REQUIRE(BusRead(snes, 0x421AU, kFirstPollEnd) == 0U);
  REQUIRE(BusRead(snes, 0x421BU, kFirstPollEnd) == 0x40U);
}

TEST_CASE("Automatic polling obeys enable transitions and holds completed results", "[unit][joypad]") {
  SNES snes;
  snes.Reset();
  auto& pad = snes.GetJoypad();
  pad.SetButton(Joypad::Button::kB, true);
  RunToTime(snes, kFirstPollEnd);
  REQUIRE(BusRead(snes, 0x4219U, kFirstPollEnd) == 0U);

  // Enabling after this frame's start does not retroactively start a read.
  BusWrite(snes, 0x4200U, 1U, kFirstPollEnd);
  const TimeMasterT next_vblank = kVblankStart + 262U * 1364U;
  RunToTime(snes, next_vblank + 130U);
  REQUIRE((BusRead(snes, 0x4212U, next_vblank + 130U) & 1U) == 0U);
  RunToTime(snes, next_vblank + 306U + 4224U);
  REQUIRE(BusRead(snes, 0x4219U, next_vblank + 306U + 4224U) == 0x80U);

  pad.SetButton(Joypad::Button::kB, false);
  BusWrite(snes, 0x4200U, 0U, next_vblank + 306U + 4224U);
  RunToTime(snes, next_vblank + 262U * 1364U + 4224U);
  REQUIRE(BusRead(snes, 0x4219U, next_vblank + 262U * 1364U + 4224U) == 0x80U);
}

TEST_CASE("Manual reads during automatic polling share the serial shift position", "[unit][joypad]") {
  SNES snes;
  snes.Reset();
  auto& pad = snes.GetJoypad();
  pad.SetButton(Joypad::Button::kB, true);
  pad.SetButton(Joypad::Button::kY, true);
  BusWrite(snes, 0x4200U, 1U);
  RunToTime(snes, kFirstPollStart + 384U);
  REQUIRE(BusRead(snes, 0x4218U, kFirstPollStart + 384U) == 1U);
  REQUIRE((BusRead(snes, 0x4016U, kFirstPollStart + 390U) & 1U) == 1U);
  RunToTime(snes, kFirstPollStart + 640U);
  REQUIRE(BusRead(snes, 0x4218U, kFirstPollStart + 640U) == 2U);

  BusWrite(snes, 0x4200U, 0U, kFirstPollStart + 640U);
  REQUIRE((BusRead(snes, 0x4212U, kFirstPollStart + 640U) & 1U) == 0U);
  RunToTime(snes, kFirstPollStart + 768U);
  REQUIRE((BusRead(snes, 0x4212U, kFirstPollStart + 768U) & 1U) == 0U);
  REQUIRE(BusRead(snes, 0x4218U, kFirstPollStart + 768U) == 2U);
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
  pad.SetButton(Joypad::Button::kY, true, 1);

  // Start a manual read mid-sequence.
  BusWrite(snes, 0x00'4016U, 0x01U);
  BusWrite(snes, 0x00'4016U, 0x00U);
  (void)BusRead(snes, 0x00'4016U);  // consume bit 0 of shift register

  snes.Reset();

  // Button state is owned by the UI/user and survives Reset.
  REQUIRE(pad.GetButton(Joypad::Button::kB));
  REQUIRE(pad.GetButton(Joypad::Button::kY, 1));

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

TEST_CASE("Auto-joypad result regs $4218-$421F use the 6-cycle MMIO timing", "[unit][joypad]") {
  // $4218-$421F live on page $42, part of the $4200-$43FF CPU/DMA register
  // block — the 6-master-cycle "fast" bus class on real hardware.
  SNES snes;
  snes.Reset();

  for (uint32_t addr = 0x00'4218U; addr <= 0x00'421FU; ++addr) {
    const BusPlan plan = snes.system_bus->Plan(addr, BusAccessType::kRead);
    REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
    REQUIRE(plan.access_cycles == 6);
  }
}
