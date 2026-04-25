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
// ALU dp,X 8-bit — one per non-BIT mnemonic (6 mnemonics × 8-bit dp,X).
// Formula 5-m+w at m=1,w=0 = 4 cycles (Bruce Clark §6.1.1.1).
// DP=0, offset=$20, X=$05 → effective address bank0:$0025.
// ============================================================================

TEST_CASE("ADC direct page indexed X 8-bit adds (DP+offset+X)", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.LoadInstruction({0x75, 0x20});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0x03, 0);
  f.ModifyRegs([](auto& r) {
    r.A = 0x0010;
    r.X = 0x0005;
    r.P.C = false;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x13);
}

TEST_CASE("AND direct page indexed X 8-bit masks with memory", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.LoadInstruction({0x35, 0x20});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0xF0, 0);
  f.ModifyRegs([](auto& r) {
    r.A = 0x00FF;
    r.X = 0x0005;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xF0);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("CMP direct page indexed X 8-bit equal sets Z and C", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.LoadInstruction({0xD5, 0x20});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0x10, 0);
  f.ModifyRegs([](auto& r) {
    r.A = 0x0010;
    r.X = 0x0005;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  // A unchanged by CMP
  REQUIRE(f.cpu.GetRegs().A == 0x0010);
}

// ============================================================================
// ALU dp,X 16-bit — one per non-BIT mnemonic (6 mnemonics × 16-bit dp,X).
// Formula 5-m+w at m=0,w=0 = 5 cycles (FLAG-05).
// ============================================================================

TEST_CASE("ADC direct page indexed X 16-bit takes 5 cycles", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.LoadInstruction({0x75, 0x20});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0x34, 0);
  f.wram.WriteRegister(0x0026, 0x12, 0);
  SetAccumulator16(f.cpu, 0x1000);
  f.ModifyRegs([](auto& r) {
    r.X = 0x0005;
    r.P.C = false;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0x2234);
}

TEST_CASE("AND direct page indexed X 16-bit", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.LoadInstruction({0x35, 0x20});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0x00, 0);
  f.wram.WriteRegister(0x0026, 0xFF, 0);
  SetAccumulator16(f.cpu, 0xFFFF);
  f.ModifyRegs([](auto& r) { r.X = 0x0005; });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0xFF00);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("CMP direct page indexed X 16-bit equal", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.LoadInstruction({0xD5, 0x20});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0x34, 0);
  f.wram.WriteRegister(0x0026, 0x12, 0);
  SetAccumulator16(f.cpu, 0x1234);
  f.ModifyRegs([](auto& r) { r.X = 0x0005; });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  // A unchanged by CMP
  REQUIRE(f.cpu.GetRegs().A == 0x1234);
}

// ============================================================================
// DL-nonzero penalty (FLAG-04). DP=$0123 → DL=$23 != 0 → +1 cycle.
// Effective address: bank0:(DP=$0123 + offset=$10 + X=$01) = bank0:$0134.
// Formula 5-m+w at m=1,w=1 = 5 cycles.
// ============================================================================

TEST_CASE("ADC direct page indexed X pays DL-nonzero penalty", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.LoadInstruction({0x75, 0x10});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.DP = 0x0123;
    r.X = 0x0001;
    r.A = 0x0001;
    r.P.C = false;
  });
  f.wram.WriteRegister(0x0134, 0x02, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 44);

  REQUIRE(r.completed_cycles == 44);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x03);
}

// ============================================================================
// BIT dp,X (0x34) — spec was registered by Plan 01-02 (MakeBitDpxSpec).
// This plan tests N/V/Z semantics and cycle counts for both widths.
// Formula 5-m+w (identical to ADC dp,X) per Bruce Clark §6.1.2.2.
// ============================================================================

TEST_CASE("BIT direct page indexed X sets N and V from memory", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.LoadInstruction({0x34, 0x20});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0xC0, 0);  // bits 7 and 6 set
  f.ModifyRegs([](auto& r) {
    r.A = 0x003F;  // A AND $C0 == 0 → Z=1
    r.X = 0x0005;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);  // m=1,w=0: 5-m+w = 4

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(f.cpu.GetRegs().P.N == true);  // bit 7 of $C0
  REQUIRE(f.cpu.GetRegs().P.V == true);  // bit 6 of $C0
  REQUIRE(f.cpu.GetRegs().P.Z == true);  // A AND $C0 == 0
  // A unchanged by BIT
  REQUIRE(f.cpu.GetRegs().A == 0x003F);
}

TEST_CASE("BIT direct page indexed X 16-bit reads N V from bit 15 and 14", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.LoadInstruction({0x34, 0x20});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0x00, 0);
  f.wram.WriteRegister(0x0026, 0xC0, 0);  // 16-bit operand = $C000 (bits 15 and 14 set)
  SetAccumulator16(f.cpu, 0x3FFF);        // A AND $C000 == 0 → Z=1
  f.ModifyRegs([](auto& r) { r.X = 0x0005; });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);  // m=0,w=0: 5-m+w = 5

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().P.N == true);  // bit 15 of $C000
  REQUIRE(f.cpu.GetRegs().P.V == true);  // bit 14 of $C000
  REQUIRE(f.cpu.GetRegs().P.Z == true);  // A AND $C000 == 0
  // A unchanged by BIT
  REQUIRE(f.cpu.GetRegs().A == 0x3FFF);
}
