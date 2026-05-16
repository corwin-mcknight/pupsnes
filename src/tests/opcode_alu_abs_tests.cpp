#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "cpu_test_fixture.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/core/device.h"
#include "pupsnes/core/scheduler.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/memory/systembus.h"
#include "pupsnes/memory/wram.h"

using namespace pupsnes;        // NOLINT(google-build-using-namespace)
using namespace pupsnes::test;  // NOLINT(google-build-using-namespace)

// ============================================================================
// ALU abs 8-bit — one per mnemonic (7 mnemonics × 8-bit abs).
// Formula 5-m at m=1 = 4 cycles (Bruce Clark §6.1.1.1, §6.1.2.2).
// ============================================================================

TEST_CASE("ADC absolute 8-bit adds DBR-banked operand", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0x6D, 0x40, 0x00});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x05, 0);
  f.ModifyRegs([](auto& r) {
    r.A = 0x000A;
    r.P.C = false;
    r.DBR = 0x7E;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x0F);
  REQUIRE(f.cpu.GetRegs().P.C == false);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
}

TEST_CASE("AND absolute 8-bit masks with memory", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0x2D, 0x60, 0x00});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0060, 0xF0, 0);
  f.ModifyRegs([](auto& r) {
    r.A = 0x00FF;
    r.DBR = 0x7E;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xF0);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("CMP absolute 8-bit equal sets Z and C", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0xCD, 0x60, 0x00});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0060, 0x10, 0);
  f.ModifyRegs([](auto& r) {
    r.A = 0x0010;
    r.DBR = 0x7E;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  REQUIRE(f.cpu.GetRegs().P.N == false);
  // A must be unchanged by CMP.
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x10);
}

TEST_CASE("BIT absolute sets N and V from memory and Z from (A AND mem)", "[unit][opcode][cpu][abs]") {
  // Operand $C0 = bits 7 and 6 both set. A=$3F has no overlap with $C0, so Z=1.
  ResetFixture f;
  f.LoadInstruction({0x2C, 0x50, 0x00});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0050, 0xC0, 0);
  f.ModifyRegs([](auto& r) {
    r.A = 0x003F;
    r.DBR = 0x7E;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(f.cpu.GetRegs().P.N == true);  // bit 7 of $C0
  REQUIRE(f.cpu.GetRegs().P.V == true);  // bit 6 of $C0
  REQUIRE(f.cpu.GetRegs().P.Z == true);  // A & $C0 = 0
  // BIT must not modify A.
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x3F);
}

TEST_CASE("BIT absolute N=0 V=1 Z=0 combination", "[unit][opcode][cpu][abs]") {
  // Operand $40 = bit 6 set, bit 7 clear. A=$40 overlaps, so Z=0.
  ResetFixture f;
  f.LoadInstruction({0x2C, 0x50, 0x00});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0050, 0x40, 0);
  f.ModifyRegs([](auto& r) {
    r.A = 0x0040;
    r.DBR = 0x7E;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(f.cpu.GetRegs().P.N == false);
  REQUIRE(f.cpu.GetRegs().P.V == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

// ============================================================================
// ALU abs 16-bit — one per mnemonic (6 ALU mnemonics × 16-bit abs).
// Formula 5-m at m=0 = 5 cycles (FLAG-05).
// BIT 16-bit covered by a separate case below.
// ============================================================================

TEST_CASE("ADC absolute 16-bit uses 5-cycle path", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0x6D, 0x40, 0x00});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x34, 0);
  f.wram.WriteRegister(0x0041, 0x12, 0);
  SetAccumulator16(f.cpu, 0x1000);
  SetDataBank(f.cpu, 0x7E);
  f.ModifyRegs([](auto& r) { r.P.C = false; });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(f.cpu.GetRegs().A == 0x2234);
}

TEST_CASE("ADC absolute 16-bit at $FFFF carries operand fetch into next bank", "[unit][opcode][cpu][abs]") {
  // Regression: ADC $FFFF with DBR=$7E must read its high byte from $7F:0000,
  // not $7E:0000. Mirrors cputest-basic test 0007 (the first failure that
  // surfaced this bug).
  ResetFixture f;
  f.LoadInstruction({0x6D, 0xFF, 0xFF});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0FFFF, 0xCB, 0);  // $7E:FFFF (low byte)
  f.wram.WriteRegister(0x10000, 0xED, 0);  // $7F:0000 (high byte)
  f.wram.WriteRegister(0x00000, 0x5A, 0);  // $7E:0000 — must NOT be the source
  SetAccumulator16(f.cpu, 0x1234);
  SetDataBank(f.cpu, 0x7E);
  f.ModifyRegs([](auto& r) { r.P.C = true; });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  // $1234 + $EDCB + 1 = $20000 -> A=$0000, Z=1, C=1, V=0, N=0.
  REQUIRE(f.cpu.GetRegs().A == 0x0000);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  REQUIRE(f.cpu.GetRegs().P.V == false);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}

TEST_CASE("AND absolute 16-bit masks wide accumulator", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0x2D, 0x40, 0x00});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x00, 0);
  f.wram.WriteRegister(0x0041, 0xFF, 0);
  SetAccumulator16(f.cpu, 0xFFFF);
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(f.cpu.GetRegs().A == 0xFF00);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("CMP absolute 16-bit equal", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0xCD, 0x40, 0x00});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x34, 0);
  f.wram.WriteRegister(0x0041, 0x12, 0);
  SetAccumulator16(f.cpu, 0x1234);
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  // A must be unchanged.
  REQUIRE(f.cpu.GetRegs().A == 0x1234);
}

TEST_CASE("BIT absolute 16-bit reads N from bit 15 and V from bit 14", "[unit][opcode][cpu][abs]") {
  // Operand $C000 = bits 15 and 14 set. A=$3FFF has no overlap with $C000, so Z=1.
  ResetFixture f;
  f.LoadInstruction({0x2C, 0x40, 0x00});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x00, 0);
  f.wram.WriteRegister(0x0041, 0xC0, 0);
  SetAccumulator16(f.cpu, 0x3FFF);
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.V == true);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().A == 0x3FFF);  // A unchanged
}

// ============================================================================
// ALU abs long 8-bit — one per mnemonic (6 ALU mnemonics × 8-bit long).
// Formula 6-m at m=1 = 5 cycles.
// ============================================================================

TEST_CASE("ADC absolute long 8-bit loads from 24-bit address", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0x6F, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0x02, 0);
  f.ModifyRegs([](auto& r) {
    r.A = 0x0001;
    r.P.C = false;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x03);
  REQUIRE(f.cpu.GetRegs().PC == 0x8004);
}

TEST_CASE("AND absolute long 8-bit", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0x2F, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0xF0, 0);
  f.ModifyRegs([](auto& r) { r.A = 0x00FF; });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xF0);
}

TEST_CASE("CMP absolute long 8-bit equal", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0xCF, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0x10, 0);
  f.ModifyRegs([](auto& r) { r.A = 0x0010; });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
}

// ============================================================================
// ALU abs long 16-bit — one per mnemonic (6 ALU mnemonics × 16-bit long).
// Formula 6-m at m=0 = 6 cycles (FLAG-05).
// ============================================================================

TEST_CASE("ADC absolute long 16-bit uses 6 cycles", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0x6F, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0x34, 0);
  f.wram.WriteRegister(0x0001, 0x12, 0);
  SetAccumulator16(f.cpu, 0x1000);
  f.ModifyRegs([](auto& r) { r.P.C = false; });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(f.cpu.GetRegs().A == 0x2234);
}

TEST_CASE("AND absolute long 16-bit", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0x2F, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0x00, 0);
  f.wram.WriteRegister(0x0001, 0xFF, 0);
  SetAccumulator16(f.cpu, 0xFFFF);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(f.cpu.GetRegs().A == 0xFF00);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("CMP absolute long 16-bit equal", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0xCF, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0x34, 0);
  f.wram.WriteRegister(0x0001, 0x12, 0);
  SetAccumulator16(f.cpu, 0x1234);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  REQUIRE(f.cpu.GetRegs().A == 0x1234);  // unchanged
}

// ============================================================================
// Loads and Compare: LDA abs, LDA long, LDX abs, LDY abs, CPX abs, CPY abs.
// ============================================================================

TEST_CASE("LDA absolute loads 8-bit value from DBR-banked address", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0xAD, 0x40, 0x00});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x42, 0);
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x42);
}

TEST_CASE("LDA absolute long loads from 24-bit address", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0xAF, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0xBB, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xBB);
}

TEST_CASE("LDX absolute 8-bit loads X low byte", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0xAE, 0x40, 0x00});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x55, 0);
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().X) == 0x55);
}

TEST_CASE("LDX absolute 16-bit uses 5-cycle path", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0xAE, 0x40, 0x00});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x34, 0);
  f.wram.WriteRegister(0x0041, 0x12, 0);
  SetIndex16X(f.cpu, 0x0000);
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(f.cpu.GetRegs().X == 0x1234);
}

TEST_CASE("LDY absolute 8-bit loads Y low byte", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0xAC, 0x40, 0x00});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x33, 0);
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().Y) == 0x33);
}

TEST_CASE("LDY absolute 16-bit uses 5-cycle path", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0xAC, 0x40, 0x00});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0xCD, 0);
  f.wram.WriteRegister(0x0041, 0xAB, 0);
  SetIndex16Y(f.cpu, 0x0000);
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(f.cpu.GetRegs().Y == 0xABCD);
}

TEST_CASE("CPX absolute equal sets Z and C", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0xEC, 0x40, 0x00});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x10, 0);
  f.ModifyRegs([](auto& r) {
    r.X = 0x0010;
    r.DBR = 0x7E;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  // X unchanged.
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().X) == 0x10);
}

TEST_CASE("CPY absolute Y > operand sets C but not Z", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.LoadInstruction({0xCC, 0x40, 0x00});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x10, 0);
  f.ModifyRegs([](auto& r) {
    r.Y = 0x0020;
    r.DBR = 0x7E;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}
