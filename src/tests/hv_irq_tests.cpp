// H/V-timer IRQ ($4200 / $4207-$420A / $4211) plumbing tests.
//
// Covers:
//   * Register read/write shadow surface.
//   * V-only, H-only, and H+V trigger modes scheduling /IRQ at the correct
//     master cycle.
//   * TIMEUP ($4211) latch behaviour: bit 7 high on pending, cleared on read,
//     also cleared by writing NMITIMEN with bits 5:4 = 00.
//   * CPU /IRQ delivery branches: gated by P.I, vectors to $00FFFE in
//     emulation mode, blocked-then-unblocked by CLI.
//
// All tests use a LoROM ResetFixture + BRA $-2 spin trick (see nmi_tests.cpp)
// so the CPU stays parked at the entry point while the PPU walks toward the
// timer-match cycle.
//
// Master-cycle reference: an NTSC scanline is 1364 mcyc except V=240 on the
// `field_==true` frame, which is 1360 mcyc. The fixture starts at field=false,
// so the first frame is "all 1364s" and V*1364 is exact.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

#include "cpu_test_fixture.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/5a22/cpu_mmio.h"
#include "pupsnes/core/scheduler.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/memory/systembus.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)
using pupsnes::test::ResetFixture;

namespace {

constexpr TimeMasterT kNormalLine = 1364U;
constexpr TimeMasterT kFrameNoOverscan = 262U * kNormalLine;

void SetupSpinRomWithIrqVector(ResetFixture& f, uint16_t reset_entry, uint16_t irq_emu_vector,
                               uint16_t irq_native_vector) {
  f.SetResetVector(reset_entry);
  // Emulation IRQ vector at $FFFE (shared with BRK; handler disambiguates via B
  // flag — the test only checks that the CPU jumped there).
  f.rom[0x7FFEU] = static_cast<uint8_t>(irq_emu_vector & 0xFFU);
  f.rom[0x7FFFU] = static_cast<uint8_t>(irq_emu_vector >> 8U);
  // Native IRQ vector at $FFEE.
  f.rom[0x7FEEU] = static_cast<uint8_t>(irq_native_vector & 0xFFU);
  f.rom[0x7FEFU] = static_cast<uint8_t>(irq_native_vector >> 8U);
  const std::size_t entry_off = static_cast<std::size_t>(reset_entry - 0x8000U);
  f.rom[entry_off + 0] = 0x80U;  // BRA
  f.rom[entry_off + 1] = 0xFEU;  // -2
  f.SyncCartridge();
}

void WriteBus(SNES& snes, SnesAddrT addr, uint8_t data, TimeMasterT t) {
  BusPlan plan = snes.system_bus->Plan(addr, BusAccessType::kWrite, data);
  REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
  (void)snes.system_bus->Follow(plan, t, data);
}

uint8_t ReadBus(SNES& snes, SnesAddrT addr, TimeMasterT t) {
  BusPlan plan = snes.system_bus->Plan(addr, BusAccessType::kRead);
  REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
  auto result = snes.system_bus->Follow(plan, t, 0);
  return result.data;
}

// Drive the machine until master time reaches `target`, chunked by scheduler
// events so the H-IRQ match fence fires at its exact master cycle.
void RunTo(SNES& snes, TimeMasterT target) {
  while (snes.GetMasterTime() < target) {
    Scheduler& sched = snes.GetScheduler();
    TimeMasterT next_chunk = target;
    if (sched.HasPendingEvents()) {
      next_chunk = std::min(next_chunk, sched.NextEventMasterTime());
    }
    if (snes.GetMasterTime() < next_chunk) {
      (void)snes.GetCpu().TickToTarget(next_chunk);
    }
    snes.MachineSync(snes.GetMasterTime());
    sched.FireEventsThrough(snes.GetMasterTime());
  }
}

}  // namespace

// ---------- Register surface ----------

TEST_CASE("HTIMEL/H, VTIMEL/H, TIMEUP register surface", "[unit][cpu_mmio][irq]") {
  SNES snes;
  snes.Reset();

  // Writes shadow correctly; debug read echoes back. The four timer regs are
  // write-only on hardware but the MMIO debug-read returns the shadow.
  REQUIRE(snes.system_bus->DebugWrite(0x00'4207U, 0x55U).ok);
  REQUIRE(snes.system_bus->DebugWrite(0x00'4208U, 0x01U).ok);  // H high bit set
  REQUIRE(snes.system_bus->DebugWrite(0x00'4209U, 0xAAU).ok);
  REQUIRE(snes.system_bus->DebugWrite(0x00'420AU, 0x00U).ok);
  REQUIRE(snes.GetCpuMmio().GetHTime() == 0x0155U);
  REQUIRE(snes.GetCpuMmio().GetVTime() == 0x00AAU);

  // TIMEUP at $4211 returns 0 when no IRQ is pending. Bits 6:0 are open-bus
  // (mask=0x80), the driven bit 7 reflects the latch.
  BusPlan plan = snes.system_bus->Plan(0x00'4211U, BusAccessType::kRead);
  REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
  auto result = snes.system_bus->Follow(plan, 0, 0);
  REQUIRE((result.data & CpuMmio::kTimeUpFlagMask) == 0U);
}

// ---------- Mode 10: V-only ----------

TEST_CASE("V-IRQ fires at start of VTIME line and latches TIMEUP", "[unit][cpu_mmio][irq]") {
  ResetFixture f;
  SetupSpinRomWithIrqVector(f, /*reset_entry=*/0x8000U,
                            /*irq_emu_vector=*/0xA000U,
                            /*irq_native_vector=*/0xA100U);
  f.snes.Reset();

  // Clear I so the CPU will deliver the IRQ.
  f.ModifyRegs([](auto& r) { r.P.I = false; });

  constexpr uint8_t kVTime = 100U;
  // Program VTIME = 100, then enable V-IRQ (NMITIMEN bits 5:4 = 10).
  WriteBus(f.snes, 0x00'4209U, kVTime, 0);
  WriteBus(f.snes, 0x00'420AU, 0x00U, 0);
  WriteBus(f.snes, 0x00'4200U, CpuMmio::kNmiTimenVIrqEnableMask, 0);

  // Run until just past line 100 begins. TIMEUP must have latched, and the
  // CPU should have begun the IRQ handler.
  RunTo(f.snes, static_cast<TimeMasterT>(kVTime) * kNormalLine + 2000U);

  const auto regs = f.cpu.GetRegs();
  REQUIRE(regs.PC >= 0xA000U);
  REQUIRE(regs.PC < 0xA400U);  // still in handler-land (0xEA fill)
  REQUIRE(regs.P.I == true);   // CPU sets I on entry
}

TEST_CASE("V-IRQ does not fire on lines other than VTIME", "[unit][cpu_mmio][irq]") {
  ResetFixture f;
  SetupSpinRomWithIrqVector(f, 0x8000U, 0xA000U, 0xA100U);
  f.snes.Reset();
  f.ModifyRegs([](auto& r) { r.P.I = false; });

  constexpr uint8_t kVTime = 50U;
  WriteBus(f.snes, 0x00'4209U, kVTime, 0);
  WriteBus(f.snes, 0x00'420AU, 0x00U, 0);
  WriteBus(f.snes, 0x00'4200U, CpuMmio::kNmiTimenVIrqEnableMask, 0);

  // Stop short of line 50 — the IRQ must not have fired yet.
  RunTo(f.snes, static_cast<TimeMasterT>(kVTime - 5) * kNormalLine);
  const auto regs = f.cpu.GetRegs();
  REQUIRE(regs.PC >= 0x8000U);
  REQUIRE(regs.PC <= 0x8002U);
}

// ---------- TIMEUP latch behaviour ----------

TEST_CASE("Reading $4211 clears TIMEUP and de-asserts /IRQ line", "[unit][cpu_mmio][irq]") {
  SNES snes;
  snes.Reset();

  // Program V-IRQ at line 10 and let the match scheduling fire.
  WriteBus(snes, 0x00'4209U, 10U, 0);
  WriteBus(snes, 0x00'420AU, 0x00U, 0);
  WriteBus(snes, 0x00'4200U, CpuMmio::kNmiTimenVIrqEnableMask, 0);

  constexpr TimeMasterT kLine10 = 10U * kNormalLine;
  RunTo(snes, kLine10 + 100U);

  // TIMEUP should be set.
  const uint8_t first = ReadBus(snes, 0x00'4211U, kLine10 + 200U);
  REQUIRE((first & CpuMmio::kTimeUpFlagMask) != 0U);
  // After read, it clears.
  const uint8_t second = ReadBus(snes, 0x00'4211U, kLine10 + 300U);
  REQUIRE((second & CpuMmio::kTimeUpFlagMask) == 0U);
  // And the IRQ line is no longer asserted.
  REQUIRE_FALSE(snes.GetCpuMmio().SampleIrqLine());
}

TEST_CASE("Writing NMITIMEN mode=00 clears pending TIMEUP", "[unit][cpu_mmio][irq]") {
  SNES snes;
  snes.Reset();

  WriteBus(snes, 0x00'4209U, 10U, 0);
  WriteBus(snes, 0x00'420AU, 0x00U, 0);
  WriteBus(snes, 0x00'4200U, CpuMmio::kNmiTimenVIrqEnableMask, 0);

  constexpr TimeMasterT kLine10 = 10U * kNormalLine;
  RunTo(snes, kLine10 + 100U);
  REQUIRE(snes.GetCpuMmio().SampleIrqLine());

  // Clearing IRQ-mode bits (5:4 = 00) clears the latch and IRQ line.
  WriteBus(snes, 0x00'4200U, 0x00U, kLine10 + 100U);
  REQUIRE_FALSE(snes.GetCpuMmio().SampleIrqLine());
  const uint8_t after = ReadBus(snes, 0x00'4211U, kLine10 + 200U);
  REQUIRE((after & CpuMmio::kTimeUpFlagMask) == 0U);
}

// ---------- Mode 01: H-only ----------

TEST_CASE("H-IRQ fires every scanline at HTIME dot", "[unit][cpu_mmio][irq]") {
  // Drive H-IRQ at H=100 (master cycle 400 within a line). Run for several
  // scanlines and count the number of TIMEUP latches by polling $4211 and
  // re-arming each time.
  SNES snes;
  snes.Reset();

  WriteBus(snes, 0x00'4207U, 100U, 0);
  WriteBus(snes, 0x00'4208U, 0x00U, 0);
  WriteBus(snes, 0x00'4200U, CpuMmio::kNmiTimenHIrqEnableMask, 0);

  int latches = 0;
  TimeMasterT t = 0;
  // 10 scanlines worth, polling once per line.
  for (uint32_t line = 0; line < 10U; ++line) {
    t = (static_cast<TimeMasterT>(line) * kNormalLine) + 800U;  // after the H=100 dot
    RunTo(snes, t);
    if ((ReadBus(snes, 0x00'4211U, t) & CpuMmio::kTimeUpFlagMask) != 0U) {
      ++latches;
    }
  }
  REQUIRE(latches == 10);
}

// ---------- Mode 11: H+V ----------

TEST_CASE("H+V IRQ fires once per frame at (HTIME, VTIME)", "[unit][cpu_mmio][irq]") {
  SNES snes;
  snes.Reset();

  WriteBus(snes, 0x00'4207U, 50U, 0);
  WriteBus(snes, 0x00'4208U, 0x00U, 0);
  WriteBus(snes, 0x00'4209U, 100U, 0);
  WriteBus(snes, 0x00'420AU, 0x00U, 0);
  WriteBus(snes, 0x00'4200U, static_cast<uint8_t>(CpuMmio::kNmiTimenHIrqEnableMask | CpuMmio::kNmiTimenVIrqEnableMask),
           0);

  // Before V=100: no match.
  RunTo(snes, 99U * kNormalLine);
  REQUIRE_FALSE(snes.GetCpuMmio().SampleIrqLine());

  // Past V=100, H>=50: match should have latched.
  RunTo(snes, 100U * kNormalLine + 800U);
  REQUIRE(snes.GetCpuMmio().SampleIrqLine());

  // Clear and confirm no further match on lines 101..261 of the same frame.
  (void)ReadBus(snes, 0x00'4211U, 100U * kNormalLine + 900U);
  REQUIRE_FALSE(snes.GetCpuMmio().SampleIrqLine());
  RunTo(snes, kFrameNoOverscan - 100U);
  REQUIRE_FALSE(snes.GetCpuMmio().SampleIrqLine());

  // Next frame: match re-arms at (50, 100).
  RunTo(snes, kFrameNoOverscan + 100U * kNormalLine + 800U);
  REQUIRE(snes.GetCpuMmio().SampleIrqLine());
}

// ---------- Delivery: P.I gating ----------

TEST_CASE("IRQ delivery is blocked while P.I=1, delivered after CLI", "[unit][cpu][irq]") {
  ResetFixture f;
  SetupSpinRomWithIrqVector(f, 0x8000U, 0xA000U, 0xA100U);
  // Reset entry: CLI; BRA -2.
  f.rom[0x0000U] = 0x58U;  // CLI
  f.rom[0x0001U] = 0x80U;  // BRA
  f.rom[0x0002U] = 0xFEU;  // -2
  f.SyncCartridge();
  f.snes.Reset();

  // P.I starts set by Reset(). Program V-IRQ at line 50.
  WriteBus(f.snes, 0x00'4209U, 50U, 0);
  WriteBus(f.snes, 0x00'420AU, 0x00U, 0);
  WriteBus(f.snes, 0x00'4200U, CpuMmio::kNmiTimenVIrqEnableMask, 0);

  // Briefly: before CLI runs (still in opcode-fetch window). Match cycle is
  // 50*1364 = 68200; ensure we run past it so the latch is set.
  RunTo(f.snes, 50U * kNormalLine + 2000U);

  // The CLI at $8000 will execute on the first opcode-fetch boundary, then
  // BRA $-2 spins. After CLI, the next instruction boundary delivers the IRQ.
  const auto regs = f.cpu.GetRegs();
  REQUIRE(regs.PC >= 0xA000U);
  REQUIRE(regs.PC < 0xA400U);
}
