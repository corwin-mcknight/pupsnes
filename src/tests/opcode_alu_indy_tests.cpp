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
// ALU / Load / Store (dp),Y — 8-bit, DP=0 (no DL penalty), Y=$10.
// Pointer at bank-0 $0010 = $1234, DBR=$7E → effective $7E:1244.
// Cycle formula 7-m+w at m=1, w=0 = 6 cycles total = 5 bus × 8 + 1 internal
// (Y-add) × 6 = 46 master cycles.
// ============================================================================

namespace {

constexpr uint8_t kDpOffset = 0x10;
constexpr uint16_t kPtrAddr = 0x0010;
constexpr uint16_t kPtrTarget = 0x1234;
constexpr uint8_t kDbr = 0x7E;
constexpr uint16_t kY = 0x0010;
constexpr uint16_t kEffectiveAddr = kPtrTarget + kY;  // 0x1244

void SetupIndirectYPointer(ResetFixture& f) {
  f.ModifyRegs([](auto& r) {
    r.DBR = kDbr;
    r.Y = kY;
  });
  f.wram.WriteRegister(kPtrAddr, 0x34, 0);
  f.wram.WriteRegister(kPtrAddr + 1, 0x12, 0);
}

}  // namespace

TEST_CASE("ADC (dp),Y 8-bit adds through indexed pointer", "[unit][opcode][cpu][indy]") {
  ResetFixture f;
  f.LoadInstruction({0x71, kDpOffset});

  f.cpu.Reset();
  SetupIndirectYPointer(f);
  f.ModifyRegs([](auto& r) {
    r.A = 0x0001;
    r.P.C = false;
  });
  f.wram.WriteRegister(kEffectiveAddr, 0x05, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x06);
}

TEST_CASE("AND (dp),Y 8-bit", "[unit][opcode][cpu][indy]") {
  ResetFixture f;
  f.LoadInstruction({0x31, kDpOffset});

  f.cpu.Reset();
  SetupIndirectYPointer(f);
  f.ModifyRegs([](auto& r) { r.A = 0x00FF; });
  f.wram.WriteRegister(kEffectiveAddr, 0xF0, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xF0);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("CMP (dp),Y 8-bit equal sets Z and C", "[unit][opcode][cpu][indy]") {
  ResetFixture f;
  f.LoadInstruction({0xD1, kDpOffset});

  f.cpu.Reset();
  SetupIndirectYPointer(f);
  f.ModifyRegs([](auto& r) { r.A = 0x0042; });
  f.wram.WriteRegister(kEffectiveAddr, 0x42, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
}

TEST_CASE("LDA (dp),Y 8-bit loads through indexed pointer", "[unit][opcode][cpu][indy]") {
  ResetFixture f;
  f.LoadInstruction({0xB1, kDpOffset});

  f.cpu.Reset();
  SetupIndirectYPointer(f);
  f.wram.WriteRegister(kEffectiveAddr, 0x99, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x99);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("STA (dp),Y 8-bit writes through indexed pointer", "[unit][opcode][cpu][indy]") {
  ResetFixture f;
  f.LoadInstruction({0x91, kDpOffset});

  f.cpu.Reset();
  SetupIndirectYPointer(f);
  f.ModifyRegs([](auto& r) { r.A = 0x0044; });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.wram.Peek(kEffectiveAddr) == 0x44);
}

// ============================================================================
// 16-bit spot-checks. 7-m+w at m=0, w=0 = 7 cycles = 6 bus × 8 + 1 internal × 6
// = 54 master cycles.
// ============================================================================

TEST_CASE("LDA (dp),Y 16-bit loads 16-bit value", "[unit][opcode][cpu][indy]") {
  ResetFixture f;
  f.LoadInstruction({0xB1, kDpOffset});

  f.cpu.Reset();
  SetupIndirectYPointer(f);
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.P.M = false;
    r.DBR = kDbr;
    r.Y = kY;
  });
  f.wram.WriteRegister(kEffectiveAddr, 0xCD, 0);
  f.wram.WriteRegister(kEffectiveAddr + 1, 0xAB, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 54);

  REQUIRE(r.completed_cycles == 54);
  REQUIRE(f.cpu.GetRegs().A == 0xABCD);
}

TEST_CASE("ADC (dp),Y 16-bit adds 16-bit value", "[unit][opcode][cpu][indy]") {
  ResetFixture f;
  f.LoadInstruction({0x71, kDpOffset});

  f.cpu.Reset();
  SetupIndirectYPointer(f);
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.P.M = false;
    r.A = 0x0001;
    r.P.C = false;
    r.DBR = kDbr;
    r.Y = kY;
  });
  f.wram.WriteRegister(kEffectiveAddr, 0x34, 0);
  f.wram.WriteRegister(kEffectiveAddr + 1, 0x12, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 54);

  REQUIRE(r.completed_cycles == 54);
  REQUIRE(f.cpu.GetRegs().A == 0x1235);
}

// ============================================================================
// Y-low overflow carries into the bank byte (bank_wrap=false in PackAddIndex).
// Pointer = $7E:FFFE, Y=$0004 → effective $7F:0002. Verifies addr_ wraps from
// $7E:FFFF into $7F:0000 instead of staying in bank $7E.
// ============================================================================

TEST_CASE("LDA (dp),Y 8-bit Y overflow carries into bank", "[unit][opcode][cpu][indy]") {
  ResetFixture f;
  f.LoadInstruction({0xB1, kDpOffset});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.DBR = kDbr;
    r.Y = 0x0004;
  });
  // Pointer at bank-0 $0010 = $7E:FFFE.
  f.wram.WriteRegister(kPtrAddr, 0xFE, 0);
  f.wram.WriteRegister(kPtrAddr + 1, 0xFF, 0);
  // Effective addr $7F:0002 = WRAM offset 0x10002 (bank $7F is the second
  // 64K of the 128K WRAM image).
  f.wram.WriteRegister(0x10002, 0x55, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x55);
}

// ============================================================================
// DL-nonzero penalty: DP=$0080 → DL=$80, +1 internal (6 master) cycle.
// 5 bus × 8 + 2 internal × 6 = 52 master.
// ============================================================================

TEST_CASE("LDA (dp),Y DL-nonzero pays penalty cycle", "[unit][opcode][cpu][indy]") {
  ResetFixture f;
  f.LoadInstruction({0xB1, kDpOffset});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.DP = 0x0080;  // DL=0x80 → +1 internal cycle
    r.DBR = kDbr;
    r.Y = kY;
  });
  // Pointer now at $0090, $0091.
  f.wram.WriteRegister(0x0090, 0x34, 0);
  f.wram.WriteRegister(0x0091, 0x12, 0);
  f.wram.WriteRegister(kEffectiveAddr, 0x77, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 52);

  REQUIRE(r.completed_cycles == 52);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x77);
}
