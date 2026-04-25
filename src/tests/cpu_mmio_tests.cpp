#include <catch2/catch_test_macros.hpp>

#include "pupsnes/hw/5a22/cpu_mmio.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)

TEST_CASE("CpuMmio maps $4200-$43FF on banks $00 and $80 as kSameClockMmio", "[unit][cpu_mmio]") {
  SNES snes;
  const auto debug_read_00 = snes.system_bus->DebugRead(0x00'4200U);
  const auto debug_read_80 = snes.system_bus->DebugRead(0x80'42FFU);
  const auto debug_read_43 = snes.system_bus->DebugRead(0x00'4300U);

  REQUIRE(debug_read_00.ok);
  REQUIRE(debug_read_80.ok);
  REQUIRE(debug_read_43.ok);
  REQUIRE(debug_read_00.device_id == snes.GetCpuMmio().GetDeviceId());
  REQUIRE(debug_read_80.device_id == snes.GetCpuMmio().GetDeviceId());
}

TEST_CASE("MEMSEL writes are stored and readable via $420D", "[unit][cpu_mmio]") {
  SNES snes;

  auto write = snes.system_bus->DebugWrite(0x00'420DU, 0x01U);
  REQUIRE(write.ok);
  REQUIRE(snes.GetCpuMmio().IsFastRomEnabled());
  REQUIRE(snes.GetCpuMmio().GetMemSel() == 0x01U);

  auto read = snes.system_bus->DebugRead(0x00'420DU);
  REQUIRE(read.ok);
  REQUIRE(read.value == 0x01U);

  // Only bit 0 matters for FASTROM behavior, but the register stores the full
  // byte so debuggers see exactly what the ROM wrote.
  auto second_write = snes.system_bus->DebugWrite(0x00'420DU, 0xFEU);
  REQUIRE(second_write.ok);
  REQUIRE_FALSE(snes.GetCpuMmio().IsFastRomEnabled());
  REQUIRE(snes.GetCpuMmio().GetMemSel() == 0xFEU);
}

TEST_CASE("CpuMmio stub registers accept writes without rejecting", "[unit][cpu_mmio]") {
  SNES snes;

  for (uint32_t reg : {0x00'4200U, 0x00'420BU, 0x00'4300U, 0x00'43FFU}) {
    auto write = snes.system_bus->DebugWrite(reg, 0xAAU);
    REQUIRE(write.ok);
  }
}

TEST_CASE("CpuMmio MEMSEL access is routed as kSameClockMmio via plan", "[unit][cpu_mmio]") {
  SNES snes;

  const BusPlan plan = snes.system_bus->Plan(0x00'420DU, BusAccessType::kRead);
  REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE(plan.target_device == snes.GetCpuMmio().GetDeviceId());
  REQUIRE(plan.access_cycles == 8);
}

TEST_CASE("HVBJOY ($4212) reports PPU VBlank bit through the bus",
          "[unit][cpu_mmio]") {
  SNES snes;
  snes.Reset();

  // Read at t=0 (v=0, h=0): neither flag set, bit 0 (auto-joypad) clear.
  BusPlan plan = snes.system_bus->Plan(0x00'4212U, BusAccessType::kRead);
  REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE(plan.target_device == snes.GetCpuMmio().GetDeviceId());
  auto result = snes.system_bus->Follow(plan, /*current_time=*/0, 0);
  REQUIRE(result.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE((result.data & CpuMmio::kHvbJoyVblankMask) == 0);
  REQUIRE((result.data & CpuMmio::kHvbJoyHblankMask) == 0);

  // Read at the start of V=225: VBlank bit set, HBlank clear (h=0).
  constexpr TimeMasterT kStartOfV225 = 225U * 1364U;
  BusPlan plan2 = snes.system_bus->Plan(0x00'4212U, BusAccessType::kRead);
  result = snes.system_bus->Follow(plan2, kStartOfV225, 0);
  REQUIRE(result.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE((result.data & CpuMmio::kHvbJoyVblankMask) != 0);
  REQUIRE((result.data & CpuMmio::kHvbJoyHblankMask) == 0);
}

TEST_CASE("RDNMI ($4210) latches bit 7 at VBlank entry and clears on read",
          "[unit][cpu_mmio]") {
  SNES snes;
  snes.Reset();

  // Before VBlank: only the CPU revision bits (0x02) are driven.
  BusPlan plan = snes.system_bus->Plan(0x00'4210U, BusAccessType::kRead);
  REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
  auto result = snes.system_bus->Follow(plan, /*current_time=*/0, 0);
  REQUIRE(result.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE((result.data & CpuMmio::kRdNmiVblankFlagMask) == 0);
  REQUIRE((result.data & CpuMmio::kRdNmiVersionMask) == CpuMmio::kRdNmiCpuVersion);

  // Inside VBlank: bit 7 set on the first read.
  constexpr TimeMasterT kStartOfV225 = 225U * 1364U;
  BusPlan plan2 = snes.system_bus->Plan(0x00'4210U, BusAccessType::kRead);
  result = snes.system_bus->Follow(plan2, kStartOfV225, 0);
  REQUIRE((result.data & CpuMmio::kRdNmiVblankFlagMask) != 0);

  // Latch clears on read — a second read in the same VBlank returns 0 on bit 7.
  BusPlan plan3 = snes.system_bus->Plan(0x00'4210U, BusAccessType::kRead);
  result = snes.system_bus->Follow(plan3, kStartOfV225 + 100, 0);
  REQUIRE((result.data & CpuMmio::kRdNmiVblankFlagMask) == 0);
  REQUIRE((result.data & CpuMmio::kRdNmiVersionMask) == CpuMmio::kRdNmiCpuVersion);
}

TEST_CASE("HVBJOY ($4212) reports HBlank bit inside the end-of-line pause",
          "[unit][cpu_mmio]") {
  SNES snes;
  snes.Reset();

  // Dot cost is 4 mcyc for H < 322. After 274 dots, h_ == 274 → HBlank set.
  BusPlan plan = snes.system_bus->Plan(0x00'4212U, BusAccessType::kRead);
  auto result = snes.system_bus->Follow(plan, /*current_time=*/274U * 4U, 0);
  REQUIRE(result.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE((result.data & CpuMmio::kHvbJoyHblankMask) != 0);
  REQUIRE((result.data & CpuMmio::kHvbJoyVblankMask) == 0);
  // Only bit 7 / bit 6 / bit 0 are driven. Other bits come from the merged
  // open-bus data so we don't assert on them.
}

TEST_CASE("CpuMmio maps pages $40 and $41 so JOYSER0/JOYSER1 reads route here",
          "[unit][cpu_mmio]") {
  SNES snes;
  // $4016 / $4017 are the legacy serial joypad ports. Without coverage the bus
  // reports these as unmapped and noise floods the bus event log on every NMI.
  const auto debug_4016 = snes.system_bus->DebugRead(0x00'4016U);
  const auto debug_4017 = snes.system_bus->DebugRead(0x00'4017U);
  REQUIRE(debug_4016.ok);
  REQUIRE(debug_4017.ok);
  REQUIRE(debug_4016.device_id == snes.GetCpuMmio().GetDeviceId());
  REQUIRE(debug_4017.device_id == snes.GetCpuMmio().GetDeviceId());
}

TEST_CASE("JOYSER0/JOYSER1 read $00 with no controller activity",
          "[unit][cpu_mmio]") {
  // Without controller emulation the manual serial-read ports must still
  // return well-defined zero so games' joypad polling code doesn't pick up
  // open-bus garbage as "buttons pressed".
  SNES snes;
  snes.Reset();

  BusPlan plan_a = snes.system_bus->Plan(0x00'4016U, BusAccessType::kRead);
  auto result_a = snes.system_bus->Follow(plan_a, /*current_time=*/0, 0);
  REQUIRE(result_a.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE(result_a.data == 0x00U);

  BusPlan plan_b = snes.system_bus->Plan(0x00'4017U, BusAccessType::kRead);
  auto result_b = snes.system_bus->Follow(plan_b, /*current_time=*/0, 0);
  REQUIRE(result_b.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE(result_b.data == 0x00U);
}

TEST_CASE("Auto-joypad result registers $4218-$421F read $00",
          "[unit][cpu_mmio]") {
  // Auto-joypad isn't yet driven, so the four 16-bit pad-state registers must
  // read all zeros — no buttons held, no controller present.
  SNES snes;
  snes.Reset();

  for (uint32_t addr = 0x4218U; addr <= 0x421FU; ++addr) {
    BusPlan plan = snes.system_bus->Plan(0x000000U | addr, BusAccessType::kRead);
    auto result = snes.system_bus->Follow(plan, /*current_time=*/0, 0);
    REQUIRE(result.outcome == BusPlanOutcome::kInlineComplete);
    REQUIRE(result.data == 0x00U);
  }
}
