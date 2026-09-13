#include <catch2/catch_test_macros.hpp>

#include "cpu_test_fixture.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/hw/5a22/cpu_mmio.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/hw/sppu/ppu_regs.h"
#include "pupsnes/memory/systembus.h"

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
  REQUIRE(plan.access_cycles == 6);  // $4200-$43FF is the 6-cycle fast bus class
}

TEST_CASE("WRIO cold-boots high and RDIO reflects its unopposed I/O lines", "[unit][cpu_mmio]") {
  SNES snes;
  snes.Reset();

  REQUIRE(snes.GetCpuMmio().GetWrio() == 0xFFU);

  BusPlan read = snes.system_bus->Plan(CpuMmio::kRdioOffset, BusAccessType::kRead);
  auto result = snes.system_bus->Follow(read, /*current_time=*/0, 0);
  REQUIRE(result.data == 0xFFU);

  BusPlan write = snes.system_bus->Plan(CpuMmio::kWrioOffset, BusAccessType::kWrite, 0x5AU);
  result = snes.system_bus->Follow(write, /*current_time=*/1, 0);
  REQUIRE(result.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE(snes.GetCpuMmio().GetWrio() == 0x5AU);

  read = snes.system_bus->Plan(CpuMmio::kRdioOffset, BusAccessType::kRead);
  result = snes.system_bus->Follow(read, /*current_time=*/2, 0);
  REQUIRE(result.data == 0x5AU);
}

TEST_CASE("WRIO bit 7 falling edge latches H/V and gates SLHV", "[unit][cpu_mmio][ppu]") {
  SNES snes;
  snes.Reset();
  Ppu& ppu = snes.GetPpu();

  // WRIO starts at $FF. Its first bit-7 falling edge at H=256 captures H/V.
  BusPlan write = snes.system_bus->Plan(CpuMmio::kWrioOffset, BusAccessType::kWrite, 0x7FU);
  (void)snes.system_bus->Follow(write, /*current_time=*/1024, 0);
  REQUIRE(ppu.GetOphct() == 256U);
  REQUIRE(ppu.GetOpvct() == 0U);
  REQUIRE(ppu.GetHvLatchFlag());

  // Holding bit 7 low does not retrigger, even after the beam advances.
  write = snes.system_bus->Plan(CpuMmio::kWrioOffset, BusAccessType::kWrite, 0x00U);
  (void)snes.system_bus->Follow(write, /*current_time=*/1028, 0);
  REQUIRE(ppu.GetOphct() == 256U);

  // Clear the status flag, then prove SLHV is inert while WRIO.7 is low.
  BusPlan read = snes.system_bus->Plan(sppu::regs::kStat78, BusAccessType::kRead);
  (void)snes.system_bus->Follow(read, /*current_time=*/1029, 0);
  read = snes.system_bus->Plan(sppu::regs::kSlhv, BusAccessType::kRead);
  (void)snes.system_bus->Follow(read, /*current_time=*/1032, 0);
  REQUIRE(ppu.GetOphct() == 256U);
  REQUIRE_FALSE(ppu.GetHvLatchFlag());

  // Raising WRIO.7 merely re-enables the gate; the next SLHV read captures.
  write = snes.system_bus->Plan(CpuMmio::kWrioOffset, BusAccessType::kWrite, 0x80U);
  (void)snes.system_bus->Follow(write, /*current_time=*/1033, 0);
  read = snes.system_bus->Plan(sppu::regs::kSlhv, BusAccessType::kRead);
  (void)snes.system_bus->Follow(read, /*current_time=*/1036, 0);
  REQUIRE(ppu.GetOphct() == 259U);
  REQUIRE(ppu.GetHvLatchFlag());
}

TEST_CASE("HVBJOY ($4212) reports PPU VBlank bit through the bus", "[unit][cpu_mmio]") {
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

TEST_CASE("RDNMI ($4210) latches bit 7 at VBlank entry and clears on read", "[unit][cpu_mmio]") {
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

TEST_CASE("HVBJOY ($4212) reports HBlank bit inside the end-of-line pause", "[unit][cpu_mmio]") {
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

TEST_CASE("CpuMmio maps pages $40 and $41 so JOYSER0/JOYSER1 reads route here", "[unit][cpu_mmio]") {
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

TEST_CASE("JOYSER0/JOYSER1 data line reads 0 when no buttons are held", "[unit][cpu_mmio]") {
  // $4016/$4017 carry pad data on bit 0. Bits 7-1 expose programmable I/O pins
  // and read as open-bus on a stock controller, so games look at bit 0 only.
  // With no buttons held, the data line is low.
  SNES snes;
  snes.Reset();

  BusPlan plan_a = snes.system_bus->Plan(0x00'4016U, BusAccessType::kRead);
  auto result_a = snes.system_bus->Follow(plan_a, /*current_time=*/0, 0);
  REQUIRE(result_a.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE((result_a.data & 0x01U) == 0x00U);

  BusPlan plan_b = snes.system_bus->Plan(0x00'4017U, BusAccessType::kRead);
  auto result_b = snes.system_bus->Follow(plan_b, /*current_time=*/0, 0);
  REQUIRE(result_b.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE((result_b.data & 0x01U) == 0x00U);
}

TEST_CASE("Auto-joypad result registers $4218-$421F read $00", "[unit][cpu_mmio]") {
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

namespace {

uint16_t MathWord(CpuMmio& mmio, uint32_t low_register) {
  const auto low = mmio.ReadRegister(low_register, 0);
  const auto high = mmio.ReadRegister(low_register + 1U, 0);
  REQUIRE(low.driven_mask == 0xFFU);
  REQUIRE(high.driven_mask == 0xFFU);
  return static_cast<uint16_t>(low.value | (static_cast<uint32_t>(high.value) << 8U));
}

void MathClocks(CpuMmio& mmio, unsigned count) {
  for (unsigned cycle = 0; cycle < count; ++cycle) mmio.ClockMathCycle();
}

}  // namespace

TEST_CASE("5A22 multiplication produces unsigned products and preserves its operands", "[unit][cpu_mmio][math]") {
  SNES snes;
  auto& mmio = snes.GetCpuMmio();
  // Independent arithmetic oracle for every pair, including unsigned values
  // above 127. Multiplication also leaves the multiplier in RDDIV.
  for (unsigned a = 0; a < 256; ++a) {
    mmio.WriteRegister(CpuMmio::kWrMpyAOffset, static_cast<uint8_t>(a), 0);
    for (unsigned b = 0; b < 256; ++b) {
      mmio.WriteRegister(CpuMmio::kWrMpyBOffset, static_cast<uint8_t>(b), 0);
      MathClocks(mmio, 8);
      REQUIRE(MathWord(mmio, CpuMmio::kRdMpyLOffset) == a * b);
      REQUIRE(MathWord(mmio, CpuMmio::kRdDivLOffset) == b);
    }
  }
}

TEST_CASE("5A22 division returns quotient and remainder including zero divisors", "[unit][cpu_mmio][math]") {
  SNES snes;
  auto& mmio = snes.GetCpuMmio();
  for (unsigned dividend : {0U, 1U, 254U, 255U, 256U, 257U, 0x3000U, 0x8000U, 0xFFFFU}) {
    mmio.WriteRegister(CpuMmio::kWrDivLOffset, static_cast<uint8_t>(dividend), 0);
    mmio.WriteRegister(CpuMmio::kWrDivHOffset, static_cast<uint8_t>(dividend >> 8U), 0);
    for (unsigned divisor = 0; divisor < 256; ++divisor) {
      mmio.WriteRegister(CpuMmio::kWrDivBOffset, static_cast<uint8_t>(divisor), 0);
      MathClocks(mmio, 16);
      const unsigned quotient = divisor == 0U ? 0xFFFFU : dividend / divisor;
      const unsigned remainder = divisor == 0U ? dividend : dividend % divisor;
      REQUIRE(MathWord(mmio, CpuMmio::kRdDivLOffset) == quotient);
      REQUIRE(MathWord(mmio, CpuMmio::kRdMpyLOffset) == remainder);
    }
  }
}

TEST_CASE("5A22 arithmetic exposes partial results and latches inputs at start", "[unit][cpu_mmio][math]") {
  SNES snes;
  auto& mmio = snes.GetCpuMmio();
  mmio.WriteRegister(CpuMmio::kWrMpyAOffset, 0xE1U, 0);
  mmio.WriteRegister(CpuMmio::kWrMpyBOffset, 3U, 0);
  REQUIRE(MathWord(mmio, CpuMmio::kRdMpyLOffset) == 0U);
  mmio.WriteRegister(CpuMmio::kWrMpyAOffset, 0U, 0);
  MathClocks(mmio, 7);
  REQUIRE(MathWord(mmio, CpuMmio::kRdMpyLOffset) == 291U);
  // Debug inspection and advancing wall/master time alone do not clock ALU.
  REQUIRE(mmio.HandleDebugRead(CpuMmio::kRdMpyLOffset) == 0x23U);
  REQUIRE(mmio.ReadRegister(CpuMmio::kRdMpyLOffset, 1000000).value == 0x23U);
  MathClocks(mmio, 1);
  REQUIRE(MathWord(mmio, CpuMmio::kRdMpyLOffset) == 675U);
  REQUIRE(MathWord(mmio, CpuMmio::kRdDivLOffset) == 3U);

  mmio.WriteRegister(CpuMmio::kWrDivLOffset, 0xFFU, 0);
  mmio.WriteRegister(CpuMmio::kWrDivHOffset, 0xFFU, 0);
  mmio.WriteRegister(CpuMmio::kWrDivBOffset, 1U, 0);
  mmio.WriteRegister(CpuMmio::kWrDivLOffset, 0U, 0);
  MathClocks(mmio, 15);
  REQUIRE(MathWord(mmio, CpuMmio::kRdMpyLOffset) == 1U);
  MathClocks(mmio, 1);
  REQUIRE(MathWord(mmio, CpuMmio::kRdDivLOffset) == 0xFFFFU);
  REQUIRE(MathWord(mmio, CpuMmio::kRdMpyLOffset) == 0U);
}

TEST_CASE("5A22 busy triggers alter the result latch without restarting arithmetic", "[unit][cpu_mmio][math]") {
  SNES snes;
  auto& mmio = snes.GetCpuMmio();
  mmio.WriteRegister(CpuMmio::kWrMpyAOffset, 0xFFU, 0);
  mmio.WriteRegister(CpuMmio::kWrMpyBOffset, 2U, 0);
  MathClocks(mmio, 7);  // 127 * 2 = 254; only the top bit remains.
  mmio.WriteRegister(CpuMmio::kWrMpyBOffset, 99U, 0);
  REQUIRE(MathWord(mmio, CpuMmio::kRdMpyLOffset) == 0U);
  MathClocks(mmio, 1);
  REQUIRE(MathWord(mmio, CpuMmio::kRdMpyLOffset) == 256U);

  mmio.WriteRegister(CpuMmio::kWrMpyBOffset, 2U, 0);
  MathClocks(mmio, 7);
  mmio.WriteRegister(CpuMmio::kWrDivLOffset, 10U, 0);
  mmio.WriteRegister(CpuMmio::kWrDivHOffset, 0U, 0);
  mmio.WriteRegister(CpuMmio::kWrDivBOffset, 5U, 0);
  MathClocks(mmio, 1);
  REQUIRE(MathWord(mmio, CpuMmio::kRdMpyLOffset) == 266U);
  MathClocks(mmio, 16);
  REQUIRE(MathWord(mmio, CpuMmio::kRdMpyLOffset) == 266U);
}

TEST_CASE("5A22 reset cancels pending arithmetic and write-only operands stay undriven", "[unit][cpu_mmio][math]") {
  SNES snes;
  auto& mmio = snes.GetCpuMmio();
  mmio.WriteRegister(CpuMmio::kWrMpyBOffset, 0xFFU, 0);
  MathClocks(mmio, 4);
  snes.Reset();
  MathClocks(mmio, 16);
  REQUIRE(MathWord(mmio, CpuMmio::kRdMpyLOffset) == 0U);
  REQUIRE(MathWord(mmio, CpuMmio::kRdDivLOffset) == 0U);
  for (uint32_t address = 0x4202U; address <= 0x4206U; ++address) {
    REQUIRE(mmio.ReadRegister(address, 0).driven_mask == 0U);
  }
  const auto write = snes.GetSystemBus().Plan(0x80'4203U, BusAccessType::kWrite, 2U);
  (void)snes.GetSystemBus().Follow(write, 0, 0);
  MathClocks(mmio, 8);
  const auto read = snes.GetSystemBus().Plan(0x00'4216U, BusAccessType::kRead);
  REQUIRE(snes.GetSystemBus().Follow(read, 0, 0).data == 0xFEU);
}

TEST_CASE("5A22 math follows CPU cycles at both ROM speeds and across scheduler slices", "[unit][cpu_mmio][math]") {
  for (bool fast : {false, true}) {
    for (bool sliced : {false, true}) {
      for (unsigned nops : {2U, 3U}) {
        test::ResetFixture fixture;
        fixture.SetRomByte(nops, 0xADU);  // LDA $4216; preceding bytes are NOPs.
        fixture.SetRomByte(nops + 1U, 0x16U);
        fixture.SetRomByte(nops + 2U, 0x42U);
        fixture.SyncCartridge();
        fixture.snes.Reset();
        fixture.ModifyRegs([](auto& regs) { regs.PBR = 0x80U; });
        auto& mmio = fixture.snes.GetCpuMmio();
        mmio.WriteRegister(CpuMmio::kMemSelOffset, fast ? 1U : 0U, 0);
        mmio.WriteRegister(CpuMmio::kWrMpyAOffset, 0xE1U, 0);
        mmio.WriteRegister(CpuMmio::kWrMpyBOffset, 3U, 0);
        const TimeMasterT rom_cycle = fast ? 6U : 8U;
        const TimeMasterT end = nops * (rom_cycle + 6U) + 3U * rom_cycle + 6U;
        if (sliced) {
          for (TimeMasterT target = 1; target <= end; ++target) (void)fixture.cpu.TickToTarget(target);
        } else {
          (void)fixture.cpu.TickToTarget(end);
        }
        // With two NOPs the read samples after seven ALU clocks; its own
        // clock completes the product only after the CPU has latched $23.
        REQUIRE(fixture.cpu.GetRegs().A == (nops == 2U ? 0x23U : 0xA3U));
        REQUIRE(MathWord(mmio, CpuMmio::kRdMpyLOffset) == 675U);
      }
    }
  }
}

TEST_CASE("5A22 arithmetic trigger writes start after their CPU clock", "[unit][cpu_mmio][math]") {
  test::ResetFixture fixture;
  fixture.LoadInstruction({0x8DU, 0x03U, 0x42U});  // STA $4203
  fixture.snes.Reset();
  fixture.ModifyRegs([](auto& regs) { regs.A = 3U; });
  auto& mmio = fixture.snes.GetCpuMmio();
  mmio.WriteRegister(CpuMmio::kWrMpyAOffset, 0xE1U, 0);
  (void)fixture.cpu.TickToTarget(30U);  // Three slow fetches and the MMIO write.
  REQUIRE(MathWord(mmio, CpuMmio::kRdMpyLOffset) == 0U);
  (void)fixture.cpu.TickToTarget(86U);  // Four NOPs = eight CPU clocks.
  REQUIRE(MathWord(mmio, CpuMmio::kRdMpyLOffset) == 675U);
}

TEST_CASE("5A22 arithmetic continues through refresh and halted CPU slices", "[unit][cpu_mmio][math]") {
  for (bool sliced : {false, true}) {
    SECTION("DRAM refresh clocks the arithmetic unit five times") {
      test::ResetFixture fixture;
      fixture.snes.Reset();
      (void)fixture.cpu.TickToTarget(540U);  // First bus cycle ending after refresh becomes due.
      auto& mmio = fixture.snes.GetCpuMmio();
      mmio.WriteRegister(CpuMmio::kWrMpyAOffset, 0xFFU, 0);
      mmio.WriteRegister(CpuMmio::kWrMpyBOffset, 1U, 0);
      if (sliced) {
        for (TimeMasterT target = 541; target <= 580; ++target) (void)fixture.cpu.TickToTarget(target);
      } else {
        (void)fixture.cpu.TickToTarget(580U);
      }
      REQUIRE(MathWord(mmio, CpuMmio::kRdMpyLOffset) == 31U);
    }
    SECTION("WAI and STP preserve the arithmetic clock phase") {
      for (uint8_t opcode : {uint8_t{0xCBU}, uint8_t{0xDBU}}) {
        test::ResetFixture fixture;
        fixture.LoadInstruction({opcode});
        fixture.snes.Reset();
        (void)fixture.cpu.TickToTarget(20U);
        auto& mmio = fixture.snes.GetCpuMmio();
        mmio.WriteRegister(CpuMmio::kWrMpyAOffset, 0xFFU, 0);
        mmio.WriteRegister(CpuMmio::kWrMpyBOffset, 1U, 0);
        if (sliced) {
          for (TimeMasterT target = 21; target <= 62; ++target) (void)fixture.cpu.TickToTarget(target);
        } else {
          (void)fixture.cpu.TickToTarget(62U);
        }
        REQUIRE(MathWord(mmio, CpuMmio::kRdMpyLOffset) == 127U);
        (void)fixture.cpu.TickToTarget(68U);
        REQUIRE(MathWord(mmio, CpuMmio::kRdMpyLOffset) == 255U);
      }
    }
  }
}

TEST_CASE("5A22 math clocks agree when a scheduler slice spans entry to WAI", "[unit][cpu_mmio][math]") {
  for (bool sliced : {false, true}) {
    test::ResetFixture fixture;
    fixture.LoadInstruction({0xCBU});
    fixture.snes.Reset();
    auto& mmio = fixture.snes.GetCpuMmio();
    mmio.WriteRegister(CpuMmio::kWrMpyAOffset, 0xFFU, 0);
    mmio.WriteRegister(CpuMmio::kWrMpyBOffset, 1U, 0);
    if (sliced) {
      for (TimeMasterT target = 1; target <= 44; ++target) (void)fixture.cpu.TickToTarget(target);
    } else {
      (void)fixture.cpu.TickToTarget(44U);
    }
    // WAI takes three clocks (20 master cycles), followed by four idle clocks.
    REQUIRE(MathWord(mmio, CpuMmio::kRdMpyLOffset) == 127U);
  }
}
