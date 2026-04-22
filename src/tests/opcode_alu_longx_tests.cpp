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
// ALU long,X 8-bit — one TEST_CASE per mnemonic (ADC/SBC/AND/ORA/EOR/CMP).
// Cycle formula 6-m at m=1 = 6 cycles (Bruce Clark §6.1.1.1).
// Base operand $7E:0000, X=$10 → effective address $7E:0010.
// ============================================================================

TEST_CASE("ADC long,X 8-bit adds indexed operand", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.LoadInstruction({0x7F, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0010, 0x05, 0);
  f.ModifyRegs([](auto& r) {
    r.X = 0x0010;
    r.A = 0x0001;
    r.P.C = false;
  });

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x06);
  REQUIRE(f.cpu.GetRegs().PC == 0x8004);
}

TEST_CASE("SBC long,X 8-bit subtracts indexed operand", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.LoadInstruction({0xFF, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0010, 0x01, 0);
  f.ModifyRegs([](auto& r) {
    r.X = 0x0010;
    r.A = 0x0010;
    r.P.C = true;
  });

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x0F);
}

TEST_CASE("AND long,X 8-bit", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.LoadInstruction({0x3F, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0010, 0xF0, 0);
  f.ModifyRegs([](auto& r) {
    r.X = 0x0010;
    r.A = 0x00FF;
  });

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xF0);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("ORA long,X 8-bit", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.LoadInstruction({0x1F, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0010, 0x0F, 0);
  f.ModifyRegs([](auto& r) {
    r.X = 0x0010;
    r.A = 0x00F0;
  });

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xFF);
}

TEST_CASE("EOR long,X 8-bit clears to zero", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.LoadInstruction({0x5F, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0010, 0xFF, 0);
  f.ModifyRegs([](auto& r) {
    r.X = 0x0010;
    r.A = 0x00FF;
  });

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x00);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

TEST_CASE("CMP long,X 8-bit equal sets Z and C", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.LoadInstruction({0xDF, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0010, 0x42, 0);
  f.ModifyRegs([](auto& r) {
    r.X = 0x0010;
    r.A = 0x0042;
  });

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  // A unchanged.
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x42);
}

// ============================================================================
// LDA/STA long,X — exercise both read and write paths. Cycle formula 6-m.
// ============================================================================

TEST_CASE("LDA long,X 8-bit loads indexed operand", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.LoadInstruction({0xBF, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0020, 0x81, 0);
  f.ModifyRegs([](auto& r) {
    r.X = 0x0020;
    r.A = 0x0000;
  });

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x81);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("STA long,X 8-bit writes indexed operand", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.LoadInstruction({0x9F, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0030, 0x00, 0);
  f.ModifyRegs([](auto& r) {
    r.X = 0x0030;
    r.A = 0x0055;
  });

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.wram.ReadRegister(0x0030, 0).value == 0x55);
}

// ============================================================================
// 16-bit M path — formula 6-m at m=0 = 7 cycles.
// ============================================================================

TEST_CASE("ADC long,X 16-bit uses 7 cycles", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.LoadInstruction({0x7F, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0x1000);
  f.ModifyRegs([](auto& r) {
    r.X = 0x0010;
    r.P.C = false;
  });
  f.wram.WriteRegister(0x0010, 0x34, 0);
  f.wram.WriteRegister(0x0011, 0x12, 0);

  TickResult r = f.cpu.Tick(54);

  REQUIRE(r.completed_cycles == 54);
  REQUIRE(f.cpu.GetRegs().A == 0x2234);
}

TEST_CASE("LDA long,X 16-bit loads wide operand", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.LoadInstruction({0xBF, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0x0000);
  f.ModifyRegs([](auto& r) { r.X = 0x0010; });
  f.wram.WriteRegister(0x0010, 0xCD, 0);
  f.wram.WriteRegister(0x0011, 0xAB, 0);

  TickResult r = f.cpu.Tick(54);

  REQUIRE(r.completed_cycles == 54);
  REQUIRE(f.cpu.GetRegs().A == 0xABCD);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

// ============================================================================
// Bank carry: base operand $7E:FFFF + X=$01 → effective $7F:0000 (carry into
// bank byte, bank_wrap=false on kAddIndexToAddr).
// ============================================================================

TEST_CASE("LDA long,X carries into bank byte on overflow", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.LoadInstruction({0xBF, 0xFF, 0xFF, 0x7E});

  f.cpu.Reset();
  f.wram.WriteRegister(0x10000U, 0x77, 0);  // $7F:0000 in WRAM linear space.
  f.ModifyRegs([](auto& r) {
    r.X = 0x0001;
    r.A = 0x0000;
  });

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x77);
}

// ============================================================================
// DBR independence: long,X ignores DBR (bank comes from the operand). Setting
// a bogus DBR must not affect the effective address.
// ============================================================================

TEST_CASE("LDA long,X ignores DBR", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.LoadInstruction({0xBF, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x33, 0);
  f.ModifyRegs([](auto& r) {
    r.DBR = 0x12;  // bogus DBR — long,X must still use operand bank.
    r.X = 0x0040;
    r.A = 0x0000;
  });

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x33);
}
