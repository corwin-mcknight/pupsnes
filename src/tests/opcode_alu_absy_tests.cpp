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
// ALU abs,Y 8-bit — one TEST_CASE per mnemonic (ADC/SBC/AND/ORA/EOR/CMP).
// Cycle formula 5-m in our always-pay-penalty lowering → at m=1, 5 cycles
// total = 4 bus × 8 + 1 internal (index-add) × 6 = 38 master cycles.
// Base operand $0100 with DBR=$7E and Y=$10 → effective address $7E:0110.
// ============================================================================

TEST_CASE("ADC abs,Y 8-bit adds DBR-banked indexed operand", "[unit][opcode][cpu][absy]") {
  ResetFixture f;
  f.LoadInstruction({0x79, 0x00, 0x01});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0110, 0x05, 0);
  f.ModifyRegs([](auto& r) {
    r.Y = 0x0010;
    r.A = 0x0001;
    r.P.C = false;
    r.DBR = 0x7E;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x06);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
}

TEST_CASE("AND abs,Y 8-bit", "[unit][opcode][cpu][absy]") {
  ResetFixture f;
  f.LoadInstruction({0x39, 0x00, 0x01});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0110, 0xF0, 0);
  f.ModifyRegs([](auto& r) {
    r.Y = 0x0010;
    r.A = 0x00FF;
    r.DBR = 0x7E;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xF0);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("CMP abs,Y 8-bit equal sets Z and C", "[unit][opcode][cpu][absy]") {
  ResetFixture f;
  f.LoadInstruction({0xD9, 0x00, 0x01});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0110, 0x42, 0);
  f.ModifyRegs([](auto& r) {
    r.Y = 0x0010;
    r.A = 0x0042;
    r.DBR = 0x7E;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
}

// ============================================================================
// LDA / LDX abs,Y — 8-bit.
// ============================================================================

TEST_CASE("LDA abs,Y 8-bit loads from DBR-banked indexed address", "[unit][opcode][cpu][absy]") {
  ResetFixture f;
  f.LoadInstruction({0xB9, 0x00, 0x01});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0110, 0x99, 0);
  f.ModifyRegs([](auto& r) {
    r.Y = 0x0010;
    r.DBR = 0x7E;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x99);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("LDX abs,Y 8-bit loads X from indexed address", "[unit][opcode][cpu][absy]") {
  ResetFixture f;
  f.LoadInstruction({0xBE, 0x00, 0x01});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0110, 0x7F, 0);
  f.ModifyRegs([](auto& r) {
    r.Y = 0x0010;
    r.DBR = 0x7E;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().X) == 0x7F);
  REQUIRE(f.cpu.GetRegs().P.N == false);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

// ============================================================================
// 16-bit spot-checks — one ALU + LDA + LDX at 16-bit width.
// Cycle formula 5-m at m=0 = 6 cycles = 5 bus × 8 + 1 internal × 6 = 46 master.
// For LDX: 5-x at x=0 = 6 cycles similarly.
// ============================================================================

TEST_CASE("ADC abs,Y 16-bit adds 16-bit indexed operand", "[unit][opcode][cpu][absy]") {
  ResetFixture f;
  f.LoadInstruction({0x79, 0x00, 0x01});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.P.M = false;
    r.Y = 0x0010;
    r.A = 0x0001;
    r.P.C = false;
    r.DBR = 0x7E;
  });
  f.wram.WriteRegister(0x0110, 0x34, 0);
  f.wram.WriteRegister(0x0111, 0x12, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0x1235);
}

TEST_CASE("LDA abs,Y 16-bit loads 16-bit value", "[unit][opcode][cpu][absy]") {
  ResetFixture f;
  f.LoadInstruction({0xB9, 0x00, 0x01});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.P.M = false;
    r.Y = 0x0010;
    r.DBR = 0x7E;
  });
  f.wram.WriteRegister(0x0110, 0xCD, 0);
  f.wram.WriteRegister(0x0111, 0xAB, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0xABCD);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("LDX abs,Y 16-bit loads 16-bit X (X=0)", "[unit][opcode][cpu][absy]") {
  ResetFixture f;
  f.LoadInstruction({0xBE, 0x00, 0x01});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.P.X = false;  // 16-bit index
    r.Y = 0x0010;
    r.DBR = 0x7E;
  });
  f.wram.WriteRegister(0x0110, 0x34, 0);
  f.wram.WriteRegister(0x0111, 0x12, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().X == 0x1234);
}

// ============================================================================
// BIT misc — BIT dp (0x24) and BIT abs,X (0x3C).
// BIT dp: 4-m+w at m=1,w=0 = 4 cycles = 32 master.
// BIT abs,X: 5-m at m=1 = 5 cycles = 38 master (4 bus × 8 + 1 internal × 6).
// BIT memory uses kBitMem semantics — N = bit7, V = bit6, Z = (A & M) == 0.
// ============================================================================

TEST_CASE("BIT dp 8-bit sets N/V from memory and Z from AND", "[unit][opcode][cpu][bit]") {
  ResetFixture f;
  f.LoadInstruction({0x24, 0x10});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) { r.A = 0x0000; });
  f.wram.WriteRegister(0x0010, 0xC0, 0);  // N=1, V=1, (A & M)=0 → Z=1

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.V == true);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

TEST_CASE("BIT dp 8-bit Z reflects AND result", "[unit][opcode][cpu][bit]") {
  ResetFixture f;
  f.LoadInstruction({0x24, 0x10});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) { r.A = 0x0001; });
  f.wram.WriteRegister(0x0010, 0x01, 0);  // N=0, V=0, (A & M)=1 → Z=0

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(f.cpu.GetRegs().P.N == false);
  REQUIRE(f.cpu.GetRegs().P.V == false);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("BIT abs,X 8-bit sets flags from indexed operand", "[unit][opcode][cpu][bit]") {
  ResetFixture f;
  f.LoadInstruction({0x3C, 0x00, 0x01});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.X = 0x0010;
    r.A = 0x00FF;
    r.DBR = 0x7E;
  });
  f.wram.WriteRegister(0x0110, 0x80, 0);  // N=1, V=0, (A & M)!=0 → Z=0

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.V == false);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}
