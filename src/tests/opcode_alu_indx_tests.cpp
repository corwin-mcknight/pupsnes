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
// ALU / Load / Store (dp,X) — 8-bit, DP=0 (no DL penalty), X=$04.
// Pointer at bank-0 ($10 + X) = $0014 = $1234, DBR=$7E → effective $7E:1234.
// Cycle formula 7-m+w at m=1, w=0 = 6 cycles total = 5 bus × 8 + 1 internal
// (X-add) × 6 = 46 master cycles.
// ============================================================================

namespace {

constexpr uint8_t kDpOffset = 0x10;
constexpr uint16_t kX = 0x0004;
constexpr uint16_t kPtrAddr = 0x0014;  // DP+offset+X
constexpr uint16_t kEffectiveAddr = 0x1234;
constexpr uint8_t kDbr = 0x7E;

void SetupIndexedIndirectXPointer(ResetFixture& f) {
  f.ModifyRegs([](auto& r) {
    r.DBR = kDbr;
    r.X = kX;
  });
  f.wram.WriteRegister(kPtrAddr, 0x34, 0);
  f.wram.WriteRegister(kPtrAddr + 1, 0x12, 0);
}

}  // namespace

TEST_CASE("ADC (dp,X) 8-bit adds through indexed pointer", "[unit][opcode][cpu][indx]") {
  ResetFixture f;
  f.LoadInstruction({0x61, kDpOffset});

  f.cpu.Reset();
  SetupIndexedIndirectXPointer(f);
  f.ModifyRegs([](auto& r) {
    r.A = 0x0001;
    r.P.C = false;
    r.X = kX;
    r.DBR = kDbr;
  });
  f.wram.WriteRegister(kEffectiveAddr, 0x05, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x06);
}

TEST_CASE("AND (dp,X) 8-bit", "[unit][opcode][cpu][indx]") {
  ResetFixture f;
  f.LoadInstruction({0x21, kDpOffset});

  f.cpu.Reset();
  SetupIndexedIndirectXPointer(f);
  f.ModifyRegs([](auto& r) {
    r.A = 0x00FF;
    r.X = kX;
    r.DBR = kDbr;
  });
  f.wram.WriteRegister(kEffectiveAddr, 0xF0, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xF0);
}

TEST_CASE("CMP (dp,X) 8-bit equal sets Z and C", "[unit][opcode][cpu][indx]") {
  ResetFixture f;
  f.LoadInstruction({0xC1, kDpOffset});

  f.cpu.Reset();
  SetupIndexedIndirectXPointer(f);
  f.ModifyRegs([](auto& r) {
    r.A = 0x0042;
    r.X = kX;
    r.DBR = kDbr;
  });
  f.wram.WriteRegister(kEffectiveAddr, 0x42, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
}

TEST_CASE("LDA (dp,X) 8-bit loads through indexed pointer", "[unit][opcode][cpu][indx]") {
  ResetFixture f;
  f.LoadInstruction({0xA1, kDpOffset});

  f.cpu.Reset();
  SetupIndexedIndirectXPointer(f);
  f.wram.WriteRegister(kEffectiveAddr, 0x99, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x99);
}

TEST_CASE("STA (dp,X) 8-bit writes through indexed pointer", "[unit][opcode][cpu][indx]") {
  ResetFixture f;
  f.LoadInstruction({0x81, kDpOffset});

  f.cpu.Reset();
  SetupIndexedIndirectXPointer(f);
  f.ModifyRegs([](auto& r) {
    r.A = 0x0033;
    r.X = kX;
    r.DBR = kDbr;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.wram.Peek(kEffectiveAddr) == 0x33);
}

// ============================================================================
// 16-bit spot-check. 7-m+w at m=0 = 7 cycles = 6 bus × 8 + 1 internal × 6 =
// 54 master cycles.
// ============================================================================

TEST_CASE("LDA (dp,X) 16-bit loads 16-bit value", "[unit][opcode][cpu][indx]") {
  ResetFixture f;
  f.LoadInstruction({0xA1, kDpOffset});

  f.cpu.Reset();
  SetupIndexedIndirectXPointer(f);
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.P.M = false;
    r.X = kX;
    r.DBR = kDbr;
  });
  f.wram.WriteRegister(kEffectiveAddr, 0xCD, 0);
  f.wram.WriteRegister(kEffectiveAddr + 1, 0xAB, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 54);

  REQUIRE(r.completed_cycles == 54);
  REQUIRE(f.cpu.GetRegs().A == 0xABCD);
}

// ============================================================================
// DL-nonzero penalty: DP=$0080, +1 internal (6 master) cycle = 52 master.
// ============================================================================

TEST_CASE("LDA (dp,X) DL-nonzero pays penalty cycle", "[unit][opcode][cpu][indx]") {
  ResetFixture f;
  f.LoadInstruction({0xA1, kDpOffset});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.DP = 0x0080;
    r.X = kX;
    r.DBR = kDbr;
  });
  // Pointer at $0080 + $10 + $04 = $0094.
  f.wram.WriteRegister(0x0094, 0x34, 0);
  f.wram.WriteRegister(0x0095, 0x12, 0);
  f.wram.WriteRegister(kEffectiveAddr, 0x77, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 52);

  REQUIRE(r.completed_cycles == 52);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x77);
}
