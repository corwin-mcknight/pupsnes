// FASTROM test suite: validates MEMSEL ($420D) plumbing end-to-end, from the
// page-table entries emitted by the LoROM mapper all the way up to the master
// cycles retired when the CPU actually executes from a fast-eligible bank.
//
// Layered by abstraction:
//   1. Page-table access_speed after LoadLoRom / MEMSEL toggles.
//   2. BusPlan::access_cycles as seen by Plan() — the CPU's contract.
//   3. CPU Tick timing — NOP from bank $80 retires 12 mcyc fast vs 14 slow.
//   4. Scope guarantees — WRAM, MMIO, slow banks untouched by MEMSEL.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/5a22/cpu_mmio.h"
#include "pupsnes/hw/cartridge.h"
#include "pupsnes/hw/device.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"
#include "systembus_test_access.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)

namespace {

constexpr std::size_t kLoRomSize = 32U * 1024U;
constexpr uint8_t kNopOpcode = 0xEAU;

// 32 KiB LoROM pre-filled with NOPs and a reset vector pointing at $8000.
// Big enough to populate fast-bank page windows with a contiguous pointer in
// MapLoRomBankRange.
std::vector<uint8_t> MakeNopLoRom() {
  std::vector<uint8_t> rom(kLoRomSize, kNopOpcode);
  rom[0x7FFCU] = 0x00U;
  rom[0x7FFDU] = 0x80U;
  return rom;
}

void WriteMemSel(SNES& snes, uint8_t value) {
  auto result = snes.system_bus->DebugWrite(0x00'420DU, value);
  REQUIRE(result.ok);
}

uint8_t GetAccessSpeed(const SNES& snes, uint8_t bank, uint8_t page) {
  return SystemBusTestAccess::GetPageEntry(*snes.system_bus, bank, page).access_speed;
}

}  // namespace

// ---------------------------------------------------------------------------
// Layer 1: page-table entries
// ---------------------------------------------------------------------------

TEST_CASE("FASTROM: fresh SNES reports FASTROM disabled", "[unit][fastrom]") {
  SNES snes;
  REQUIRE_FALSE(snes.GetCpuMmio().IsFastRomEnabled());
  REQUIRE(snes.GetCpuMmio().GetMemSel() == 0x00U);
}

TEST_CASE("FASTROM: MEMSEL bit 0 drives IsFastRomEnabled; other bits are ignored", "[unit][fastrom]") {
  SNES snes;

  WriteMemSel(snes, 0xFFU);
  REQUIRE(snes.GetCpuMmio().IsFastRomEnabled());

  WriteMemSel(snes, 0xFEU);
  REQUIRE_FALSE(snes.GetCpuMmio().IsFastRomEnabled());

  WriteMemSel(snes, 0x01U);
  REQUIRE(snes.GetCpuMmio().IsFastRomEnabled());
}

TEST_CASE("FASTROM: LoROM starts mapped with slow 8-cycle fast-bank pages", "[unit][fastrom]") {
  SNES snes;
  auto rom = MakeNopLoRom();
  snes.LoadLoRom(rom);

  for (uint8_t bank : {uint8_t{0x80U}, uint8_t{0xC0U}, uint8_t{0xFDU}}) {
    REQUIRE(GetAccessSpeed(snes, bank, 0x80U) == 8);
  }
}

TEST_CASE("FASTROM: enabling MEMSEL flips banks $80-$FD pages $80-$FF to 6 mcyc", "[unit][fastrom]") {
  SNES snes;
  auto rom = MakeNopLoRom();
  snes.LoadLoRom(rom);
  WriteMemSel(snes, 0x01U);

  for (uint8_t bank : {uint8_t{0x80U}, uint8_t{0xC0U}, uint8_t{0xFDU}}) {
    for (uint8_t page : {uint8_t{0x80U}, uint8_t{0xC0U}, uint8_t{0xFFU}}) {
      REQUIRE(GetAccessSpeed(snes, bank, page) == 6);
    }
  }
}

TEST_CASE("FASTROM: disabling MEMSEL reverts fast banks to 8 mcyc", "[unit][fastrom]") {
  SNES snes;
  auto rom = MakeNopLoRom();
  snes.LoadLoRom(rom);

  WriteMemSel(snes, 0x01U);
  REQUIRE(GetAccessSpeed(snes, 0x80U, 0x80U) == 6);

  WriteMemSel(snes, 0x00U);
  REQUIRE(GetAccessSpeed(snes, 0x80U, 0x80U) == 8);
}

// ---------------------------------------------------------------------------
// Layer 2: scope — what MEMSEL must NOT touch
// ---------------------------------------------------------------------------

TEST_CASE("FASTROM: slow banks $00-$7D never become fast", "[unit][fastrom]") {
  SNES snes;
  auto rom = MakeNopLoRom();
  snes.LoadLoRom(rom);
  WriteMemSel(snes, 0x01U);

  for (uint8_t bank : {uint8_t{0x00U}, uint8_t{0x40U}, uint8_t{0x7DU}}) {
    REQUIRE(GetAccessSpeed(snes, bank, 0x80U) == 8);
  }
}

TEST_CASE("FASTROM: WRAM pages stay at 8 mcyc regardless of MEMSEL", "[unit][fastrom]") {
  SNES snes;
  auto rom = MakeNopLoRom();
  snes.LoadLoRom(rom);
  WriteMemSel(snes, 0x01U);

  // WRAM proper at $7E / $7F.
  REQUIRE(GetAccessSpeed(snes, 0x7EU, 0x00U) == 8);
  REQUIRE(GetAccessSpeed(snes, 0x7FU, 0xFFU) == 8);
  // LowRAM mirror under banks $00-$3F / $80-$BF pages $00-$1F — sits inside the
  // fast-bank range but is WRAM-backed, so MEMSEL must not touch it.
  REQUIRE(GetAccessSpeed(snes, 0x80U, 0x00U) == 8);
  REQUIRE(GetAccessSpeed(snes, 0x80U, 0x1FU) == 8);
}

TEST_CASE("FASTROM: CPU MMIO pages ($42/$43) stay at 8 mcyc", "[unit][fastrom]") {
  SNES snes;
  auto rom = MakeNopLoRom();
  snes.LoadLoRom(rom);
  WriteMemSel(snes, 0x01U);

  REQUIRE(GetAccessSpeed(snes, 0x00U, 0x42U) == 8);
  REQUIRE(GetAccessSpeed(snes, 0x00U, 0x43U) == 8);
  REQUIRE(GetAccessSpeed(snes, 0x80U, 0x42U) == 8);
  REQUIRE(GetAccessSpeed(snes, 0x80U, 0x43U) == 8);
}

// ---------------------------------------------------------------------------
// Layer 3: BusPlan contract — what the CPU sees from Plan()
// ---------------------------------------------------------------------------

TEST_CASE("FASTROM: BusPlan access_cycles drops from 8 to 6 on fast-bank fetches", "[unit][fastrom]") {
  SNES snes;
  auto rom = MakeNopLoRom();
  snes.LoadLoRom(rom);

  BusPlan plan_slow = snes.system_bus->Plan(0x80'8000U, BusAccessType::kRead);
  REQUIRE(plan_slow.access_cycles == 8);
  REQUIRE(plan_slow.outcome == BusPlanOutcome::kInlineComplete);

  WriteMemSel(snes, 0x01U);

  BusPlan plan_fast = snes.system_bus->Plan(0x80'8000U, BusAccessType::kRead);
  REQUIRE(plan_fast.access_cycles == 6);
  REQUIRE(plan_fast.outcome == BusPlanOutcome::kInlineComplete);
}

TEST_CASE("FASTROM: BusPlan for slow-bank fetch is unaffected by MEMSEL", "[unit][fastrom]") {
  SNES snes;
  auto rom = MakeNopLoRom();
  snes.LoadLoRom(rom);
  WriteMemSel(snes, 0x01U);

  BusPlan plan = snes.system_bus->Plan(0x00'8000U, BusAccessType::kRead);
  REQUIRE(plan.access_cycles == 8);
}

// ---------------------------------------------------------------------------
// Layer 4: CPU Tick timing — the user-visible payoff
// ---------------------------------------------------------------------------

namespace {

void JumpToFastBank(CPU& cpu) {
  auto regs = cpu.GetRegs();
  regs.PBR = 0x80U;
  regs.PC = 0x8000U;
  cpu.SetRegs(regs);
}

}  // namespace

TEST_CASE("FASTROM: a NOP in a fast bank retires 12 mcyc when MEMSEL=1", "[unit][fastrom]") {
  // NOP = 1 bus fetch (access_speed mcyc) + 1 internal cycle (6 mcyc).
  // Slow bank: 8 + 6 = 14. Fast bank with MEMSEL=1: 6 + 6 = 12.
  SNES snes;
  auto rom = MakeNopLoRom();
  snes.LoadLoRom(rom);
  WriteMemSel(snes, 0x01U);

  CPU& cpu = snes.GetCpu();
  cpu.Reset();
  JumpToFastBank(cpu);

  TickResult r = cpu.TickToTarget(snes.GetMasterTime() + 12);
  REQUIRE(r.completed_cycles == 12);
  REQUIRE(cpu.GetRegs().PC == 0x8001U);
}

TEST_CASE("FASTROM: the same NOP retires 14 mcyc when MEMSEL=0", "[unit][fastrom]") {
  SNES snes;
  auto rom = MakeNopLoRom();
  snes.LoadLoRom(rom);
  // MEMSEL left at its reset default (0).

  CPU& cpu = snes.GetCpu();
  cpu.Reset();
  JumpToFastBank(cpu);

  TickResult r = cpu.TickToTarget(snes.GetMasterTime() + 14);
  REQUIRE(r.completed_cycles == 14);
  REQUIRE(cpu.GetRegs().PC == 0x8001U);
}

TEST_CASE("FASTROM: toggling MEMSEL mid-run changes retirement cost on the next NOP", "[unit][fastrom]") {
  SNES snes;
  auto rom = MakeNopLoRom();
  snes.LoadLoRom(rom);

  CPU& cpu = snes.GetCpu();
  cpu.Reset();
  JumpToFastBank(cpu);

  // First NOP, slow: 14 mcyc.
  TickResult slow = cpu.TickToTarget(snes.GetMasterTime() + 14);
  REQUIRE(slow.completed_cycles == 14);
  REQUIRE(cpu.GetRegs().PC == 0x8001U);

  WriteMemSel(snes, 0x01U);

  // Second NOP, fast: 12 mcyc.
  TickResult fast = cpu.TickToTarget(snes.GetMasterTime() + 12);
  REQUIRE(fast.completed_cycles == 12);
  REQUIRE(cpu.GetRegs().PC == 0x8002U);
}

// ---------------------------------------------------------------------------
// Layer 5: edge cases
// ---------------------------------------------------------------------------

TEST_CASE("FASTROM: MEMSEL writes are safe before any ROM is mapped", "[unit][fastrom]") {
  // OnMemSelChanged is a no-op when lorom_mapped_ is false; verify no crash
  // and that the stored bit is still readable.
  SNES snes;
  WriteMemSel(snes, 0x01U);
  REQUIRE(snes.GetCpuMmio().IsFastRomEnabled());
}

TEST_CASE("FASTROM: Cartridge reports MapperKind::kLoROM after LoadLoRom", "[unit][fastrom]") {
  SNES snes;
  REQUIRE(snes.GetCartridge().GetMapperKind() == MapperKind::kNone);

  auto rom = MakeNopLoRom();
  snes.LoadLoRom(rom);
  REQUIRE(snes.GetCartridge().GetMapperKind() == MapperKind::kLoROM);
}

// ---------------------------------------------------------------------------
// Reset / cartridge-swap semantics
// ---------------------------------------------------------------------------

TEST_CASE("FASTROM: SNES::Reset clears MEMSEL and remaps fast banks to slow", "[unit][fastrom]") {
  SNES snes;
  auto rom = MakeNopLoRom();
  snes.LoadLoRom(rom);
  WriteMemSel(snes, 0x01U);
  REQUIRE(snes.GetCpuMmio().IsFastRomEnabled());
  REQUIRE(GetAccessSpeed(snes, 0x80U, 0x80U) == 6);

  snes.Reset();

  REQUIRE_FALSE(snes.GetCpuMmio().IsFastRomEnabled());
  REQUIRE(snes.GetCpuMmio().GetMemSel() == 0x00U);
  REQUIRE(GetAccessSpeed(snes, 0x80U, 0x80U) == 8);
}

TEST_CASE("FASTROM: LoadLoRom clears MEMSEL from a prior cartridge", "[unit][fastrom]") {
  SNES snes;
  auto rom1 = MakeNopLoRom();
  snes.LoadLoRom(rom1);
  WriteMemSel(snes, 0x01U);
  REQUIRE(snes.GetCpuMmio().IsFastRomEnabled());

  auto rom2 = MakeNopLoRom();
  snes.LoadLoRom(rom2);

  REQUIRE_FALSE(snes.GetCpuMmio().IsFastRomEnabled());
  REQUIRE(GetAccessSpeed(snes, 0x80U, 0x80U) == 8);
}

// ---------------------------------------------------------------------------
// Regression: a CatchUpDevice-driven MEMSEL write must not cause any
// invariant violations. Driving many TickToTarget calls past the MEMSEL
// write exercises that state.
// ---------------------------------------------------------------------------

TEST_CASE("FASTROM: running through sta $420D does not cause invariant violations", "[unit][fastrom]") {
  SNES snes;

  // LDA #$01 / STA f:$00420D / BRA self.
  std::vector<uint8_t> rom(kLoRomSize, kNopOpcode);
  rom[0x0000U] = 0xA9U;  // LDA #$01
  rom[0x0001U] = 0x01U;
  rom[0x0002U] = 0x8FU;  // STA f:$00420D
  rom[0x0003U] = 0x0DU;
  rom[0x0004U] = 0x42U;
  rom[0x0005U] = 0x00U;
  rom[0x0006U] = 0x80U;  // BRA $FE
  rom[0x0007U] = 0xFEU;
  rom[0x7FFCU] = 0x00U;
  rom[0x7FFDU] = 0x80U;
  snes.LoadLoRom(rom);
  snes.Reset();

  // Drive CPU through many short slices, passing through the STA $420D
  // write and the BRA self loop, without needing scheduler Step machinery.
  for (int i = 0; i < 200; ++i) {
    const TimeMasterT target = snes.GetMasterTime() + 20;
    (void)snes.GetCpu().TickToTarget(target);
  }

  REQUIRE(snes.GetCpuMmio().IsFastRomEnabled());
}
