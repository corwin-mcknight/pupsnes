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
