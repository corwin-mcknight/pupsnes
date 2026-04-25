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
// ALU sr,S 8-bit — one TEST_CASE per ALU mnemonic (ADC/SBC/AND/ORA/EOR/CMP).
// Cycle formula 5-m at m=1 = 4 cycles (Bruce Clark §6.1.1.1). No DL penalty.
// SP=0x01F0, offset=$04 → effective address bank0:$01F4.
// ============================================================================

TEST_CASE("ADC stack-relative 8-bit adds from bank-0 SP+offset", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.LoadInstruction({0x63, 0x04});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.SP = 0x01F0;
    r.A = 0x0001;
    r.P.C = false;
  });
  f.wram.WriteRegister(0x01F4, 0x05, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x06);
  REQUIRE(f.cpu.GetRegs().P.C == false);
}

TEST_CASE("AND stack-relative 8-bit", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.LoadInstruction({0x23, 0x04});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.SP = 0x01F0;
    r.A = 0x00FF;
  });
  f.wram.WriteRegister(0x01F4, 0xF0, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xF0);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("CMP stack-relative 8-bit equal result", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.LoadInstruction({0xC3, 0x04});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.SP = 0x01F0;
    r.A = 0x0010;
  });
  f.wram.WriteRegister(0x01F4, 0x10, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  // A is unchanged by CMP.
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x10);
}

// ============================================================================
// ALU sr,S 16-bit — one TEST_CASE per ALU mnemonic (FLAG-05).
// Cycle formula 5-m at m=0 = 5 cycles.
// ============================================================================

TEST_CASE("ADC stack-relative 16-bit uses 5 cycles", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.LoadInstruction({0x63, 0x04});

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0x1000);
  f.ModifyRegs([](auto& r) {
    r.SP = 0x01F0;
    r.P.C = false;
  });
  f.wram.WriteRegister(0x01F4, 0x34, 0);
  f.wram.WriteRegister(0x01F5, 0x12, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0x2234);
}

TEST_CASE("AND stack-relative 16-bit", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.LoadInstruction({0x23, 0x04});

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0xFFFF);
  f.ModifyRegs([](auto& r) { r.SP = 0x01F0; });
  f.wram.WriteRegister(0x01F4, 0x00, 0);
  f.wram.WriteRegister(0x01F5, 0xFF, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0xFF00);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("CMP stack-relative 16-bit equal", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.LoadInstruction({0xC3, 0x04});

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0x1234);
  f.ModifyRegs([](auto& r) { r.SP = 0x01F0; });
  f.wram.WriteRegister(0x01F4, 0x34, 0);
  f.wram.WriteRegister(0x01F5, 0x12, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  // A unchanged.
  REQUIRE(f.cpu.GetRegs().A == 0x1234);
}

// ============================================================================
// sr,S does NOT pay the DL-nonzero penalty (T-04-02). Even with DP=$0123
// (DL=$23 != 0) the cycle count remains 4 for the 8-bit form, not 5.
// ============================================================================

TEST_CASE("ADC stack-relative does not pay DL-nonzero penalty", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.LoadInstruction({0x63, 0x04});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.SP = 0x01F0;
    r.A = 0x0001;
    r.DP = 0x0123;  // DL = 0x23 != 0 — would trigger +w for dp modes, but not for sr,S.
    r.P.C = false;
  });
  f.wram.WriteRegister(0x01F4, 0x05, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x06);
}
