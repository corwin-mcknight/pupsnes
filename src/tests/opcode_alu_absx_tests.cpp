#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "cpu_test_fixture.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/cartridge.h"
#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"
#include "pupsnes/hw/wram.h"

using namespace pupsnes;        // NOLINT(google-build-using-namespace)
using namespace pupsnes::test;  // NOLINT(google-build-using-namespace)

// ============================================================================
// ALU abs,X 8-bit — one TEST_CASE per mnemonic (ADC/SBC/AND/ORA/EOR/CMP).
// Cycle formula 5-m (our always-pay-penalty lowering) at m=1 = 5 cycles
// (Bruce Clark §6.1.1.1, "4-m+x+x*p" with x=1/p=1 fast-path collapsed to the
// unconditional penalty — we overcount p=0 cases by 1 cycle deliberately).
// Base operand $0100 with DBR=$7E and X=$10 → effective address $7E:0110.
// ============================================================================

TEST_CASE("ADC abs,X 8-bit adds DBR-banked indexed operand", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.LoadInstruction({0x7D, 0x00, 0x01});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0110, 0x05, 0);
  f.ModifyRegs([](auto& r) {
    r.X = 0x0010;
    r.A = 0x0001;
    r.P.C = false;
    r.DBR = 0x7E;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x06);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
}

TEST_CASE("AND abs,X 8-bit", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.LoadInstruction({0x3D, 0x00, 0x01});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0110, 0xF0, 0);
  f.ModifyRegs([](auto& r) {
    r.X = 0x0010;
    r.A = 0x00FF;
    r.DBR = 0x7E;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xF0);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("CMP abs,X 8-bit equal sets Z and C", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.LoadInstruction({0xDD, 0x00, 0x01});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0110, 0x42, 0);
  f.ModifyRegs([](auto& r) {
    r.X = 0x0010;
    r.A = 0x0042;
    r.DBR = 0x7E;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  // A unchanged.
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x42);
}

// ============================================================================
// LDA/LDY abs,X — exercise the A-width and X-width load paths.
// LDA formula 5-m (our lowering); LDY formula 5-x.
// ============================================================================

TEST_CASE("LDA abs,X 8-bit loads indexed operand", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.LoadInstruction({0xBD, 0x00, 0x01});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0120, 0x81, 0);
  f.ModifyRegs([](auto& r) {
    r.X = 0x0020;
    r.A = 0x0000;
    r.DBR = 0x7E;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x81);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("LDY abs,X 8-bit loads indexed operand into Y", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.LoadInstruction({0xBC, 0x00, 0x01});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0130, 0x42, 0);
  f.ModifyRegs([](auto& r) {
    r.X = 0x0030;
    r.Y = 0x0000;
    r.DBR = 0x7E;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().Y) == 0x42);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}

// ============================================================================
// 16-bit M path — formula 5-m at m=0 = 6 cycles = 46 master cycles.
// ============================================================================

TEST_CASE("ADC abs,X 16-bit adds 16-bit operand", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.LoadInstruction({0x7D, 0x00, 0x01});

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0x1000);
  f.ModifyRegs([](auto& r) {
    r.X = 0x0010;
    r.P.C = false;
    r.DBR = 0x7E;
  });
  f.wram.WriteRegister(0x0110, 0x34, 0);
  f.wram.WriteRegister(0x0111, 0x12, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0x2234);
}

TEST_CASE("LDA abs,X 16-bit loads wide operand", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.LoadInstruction({0xBD, 0x00, 0x01});

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0x0000);
  f.ModifyRegs([](auto& r) {
    r.X = 0x0010;
    r.DBR = 0x7E;
  });
  f.wram.WriteRegister(0x0110, 0xCD, 0);
  f.wram.WriteRegister(0x0111, 0xAB, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0xABCD);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

// ============================================================================
// 16-bit X path — LDY abs,X with X=0 (16-bit indices): formula 5-x at x=0 = 5,
// loading 16 bits into Y takes one extra cycle (6 cycles total = 46 masters).
// ============================================================================

TEST_CASE("LDY abs,X 16-bit loads wide operand into Y", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.LoadInstruction({0xBC, 0x00, 0x01});

  f.cpu.Reset();
  SetIndex16X(f.cpu, 0x0010);
  f.ModifyRegs([](auto& r) {
    r.Y = 0x0000;
    r.DBR = 0x7E;
  });
  f.wram.WriteRegister(0x0110, 0x34, 0);
  f.wram.WriteRegister(0x0111, 0x12, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().Y == 0x1234);
}

// ============================================================================
// Bank carry on index add: base operand $FFF0 with DBR=$7E and X=$20 →
// effective $7F:0010 (low-16 overflow carries into bank byte).
// ============================================================================

TEST_CASE("LDA abs,X carries into bank byte on index overflow", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.LoadInstruction({0xBD, 0xF0, 0xFF});

  f.cpu.Reset();
  f.wram.WriteRegister(0x10010U, 0x77, 0);  // $7F:0010 in WRAM linear space.
  f.ModifyRegs([](auto& r) {
    r.X = 0x0020;
    r.A = 0x0000;
    r.DBR = 0x7E;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x77);
}
