#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "cpu_test_fixture.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/core/device.h"
#include "pupsnes/core/scheduler.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/memory/systembus.h"
#include "pupsnes/memory/wram.h"

using namespace pupsnes;        // NOLINT(google-build-using-namespace)
using namespace pupsnes::test;  // NOLINT(google-build-using-namespace)

// ============================================================================
// ALU / Load / Store [dp],Y — 8-bit, DP=0 (no DL penalty), Y=$10.
// 24-bit pointer at bank-0 $0040 = $7E:3010, Y=$10 → effective $7E:3020.
// Cycle formula 7-m+w at m=1, w=0 = 6 cycles total = 6 bus × 8 = 48 master.
// (Y add folded into the bank-fetch cycle — no extra internal slot.)
// ============================================================================

namespace {

constexpr uint8_t kDpOffset = 0x40;
constexpr uint16_t kPtrAddr = 0x0040;
constexpr uint8_t kPtrBank = 0x7E;
constexpr uint16_t kPtrOffset = 0x3010;
constexpr uint16_t kY = 0x0010;
constexpr uint16_t kEffectiveAddr = kPtrOffset + kY;  // 0x3020 within bank $7E

void SetupLongIndirectYPointer(ResetFixture& f) {
  f.ModifyRegs([](auto& r) { r.Y = kY; });
  f.wram.WriteRegister(kPtrAddr, 0x10, 0);
  f.wram.WriteRegister(kPtrAddr + 1, 0x30, 0);
  f.wram.WriteRegister(kPtrAddr + 2, kPtrBank, 0);
}

}  // namespace

TEST_CASE("ADC [dp],Y 8-bit adds through long indexed pointer", "[unit][opcode][cpu][lindy]") {
  ResetFixture f;
  f.LoadInstruction({0x77, kDpOffset});

  f.cpu.Reset();
  SetupLongIndirectYPointer(f);
  f.ModifyRegs([](auto& r) {
    r.A = 0x0001;
    r.P.C = false;
    r.Y = kY;
  });
  f.wram.WriteRegister(kEffectiveAddr, 0x10, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x11);
}

TEST_CASE("AND [dp],Y 8-bit", "[unit][opcode][cpu][lindy]") {
  ResetFixture f;
  f.LoadInstruction({0x37, kDpOffset});

  f.cpu.Reset();
  SetupLongIndirectYPointer(f);
  f.ModifyRegs([](auto& r) {
    r.A = 0x00AA;
    r.Y = kY;
  });
  f.wram.WriteRegister(kEffectiveAddr, 0x0F, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x0A);
}

TEST_CASE("CMP [dp],Y 8-bit less-than clears C", "[unit][opcode][cpu][lindy]") {
  ResetFixture f;
  f.LoadInstruction({0xD7, kDpOffset});

  f.cpu.Reset();
  SetupLongIndirectYPointer(f);
  f.ModifyRegs([](auto& r) {
    r.A = 0x0001;
    r.Y = kY;
  });
  f.wram.WriteRegister(kEffectiveAddr, 0x10, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(f.cpu.GetRegs().P.C == false);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("LDA [dp],Y 8-bit loads through long indexed pointer", "[unit][opcode][cpu][lindy]") {
  ResetFixture f;
  f.LoadInstruction({0xB7, kDpOffset});

  f.cpu.Reset();
  SetupLongIndirectYPointer(f);
  f.wram.WriteRegister(kEffectiveAddr, 0x77, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x77);
}

TEST_CASE("STA [dp],Y 8-bit writes through long indexed pointer", "[unit][opcode][cpu][lindy]") {
  ResetFixture f;
  f.LoadInstruction({0x97, kDpOffset});

  f.cpu.Reset();
  SetupLongIndirectYPointer(f);
  f.ModifyRegs([](auto& r) {
    r.A = 0x0066;
    r.Y = kY;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(f.wram.Peek(kEffectiveAddr) == 0x66);
}

// ============================================================================
// 16-bit spot-check. 7-m+w at m=0 = 7 cycles = 56 master cycles.
// ============================================================================

TEST_CASE("LDA [dp],Y 16-bit loads 16-bit value", "[unit][opcode][cpu][lindy]") {
  ResetFixture f;
  f.LoadInstruction({0xB7, kDpOffset});

  f.cpu.Reset();
  SetupLongIndirectYPointer(f);
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.P.M = false;
    r.Y = kY;
  });
  f.wram.WriteRegister(kEffectiveAddr, 0xCD, 0);
  f.wram.WriteRegister(kEffectiveAddr + 1, 0xAB, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 56);

  REQUIRE(r.completed_cycles == 56);
  REQUIRE(f.cpu.GetRegs().A == 0xABCD);
}

// ============================================================================
// Y add carries into bank byte. Pointer = $7E:FFFE, Y=$0004 → effective
// $7F:0002. Verifies the inline Y add in the bank-fetch slot uses 24-bit
// arithmetic (bank_wrap=false equivalent).
// ============================================================================

TEST_CASE("LDA [dp],Y 8-bit Y overflow carries into bank", "[unit][opcode][cpu][lindy]") {
  ResetFixture f;
  f.LoadInstruction({0xB7, kDpOffset});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) { r.Y = 0x0004; });
  f.wram.WriteRegister(kPtrAddr, 0xFE, 0);
  f.wram.WriteRegister(kPtrAddr + 1, 0xFF, 0);
  f.wram.WriteRegister(kPtrAddr + 2, kPtrBank, 0);
  // Effective addr $7F:0002 = WRAM offset 0x10002.
  f.wram.WriteRegister(0x10002, 0x42, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x42);
}

// ============================================================================
// DL-nonzero penalty: DP=$0080, +1 internal (6 master) = 54 master cycles.
// ============================================================================

TEST_CASE("LDA [dp],Y DL-nonzero pays penalty cycle", "[unit][opcode][cpu][lindy]") {
  ResetFixture f;
  f.LoadInstruction({0xB7, kDpOffset});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.DP = 0x0080;
    r.Y = kY;
  });
  // Pointer now at $00C0, $00C1, $00C2 (DP=$80 + offset $40).
  f.wram.WriteRegister(0x00C0, 0x10, 0);
  f.wram.WriteRegister(0x00C1, 0x30, 0);
  f.wram.WriteRegister(0x00C2, kPtrBank, 0);
  f.wram.WriteRegister(kEffectiveAddr, 0x55, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 54);

  REQUIRE(r.completed_cycles == 54);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x55);
}
