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
// ALU / Load / Store (sr,S),Y — Bruce Clark §5.21 / §6.1.1.1.
// Cycle formula 8-m at m=1 = 7 cycles = 6 bus × 8 + 1 internal × 6 = 54
// master cycles. SP=$01F0, offset=$04 → pointer at bank 0:$01F4 = $1234.
// DBR=$7E, Y=$10 → effective address $7E:1244.
// ============================================================================

namespace {

constexpr uint8_t kSrOffset = 0x04;
constexpr uint16_t kPtrAddr = 0x01F4;
constexpr uint16_t kPtrTarget = 0x1234;
constexpr uint8_t kDbr = 0x7E;
constexpr uint16_t kY = 0x0010;
constexpr uint16_t kEffectiveAddr = kPtrTarget + kY;  // $1244

void SetupPointer(ResetFixture& f) {
  f.ModifyRegs([](auto& r) {
    r.SP = 0x01F0;
    r.DBR = kDbr;
    r.Y = kY;
  });
  f.wram.WriteRegister(kPtrAddr, 0x34, 0);
  f.wram.WriteRegister(kPtrAddr + 1, 0x12, 0);
}

}  // namespace

TEST_CASE("LDA (sr,S),Y 8-bit reads via stack pointer + Y", "[unit][opcode][cpu][sr-indy]") {
  ResetFixture f;
  f.LoadInstruction({0xB3, kSrOffset});

  f.cpu.Reset();
  SetupPointer(f);
  f.wram.WriteRegister(kEffectiveAddr, 0x5A, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 54);

  REQUIRE(r.completed_cycles == 54);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x5A);
}

TEST_CASE("STA (sr,S),Y 8-bit writes via stack pointer + Y", "[unit][opcode][cpu][sr-indy]") {
  ResetFixture f;
  f.LoadInstruction({0x93, kSrOffset});

  f.cpu.Reset();
  SetupPointer(f);
  f.ModifyRegs([](auto& r) { r.A = 0x00A5; });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 54);

  REQUIRE(r.completed_cycles == 54);
  REQUIRE(f.wram.ReadRegister(kEffectiveAddr, 0).value == 0xA5);
}

TEST_CASE("ADC (sr,S),Y 8-bit adds via stack indirect", "[unit][opcode][cpu][sr-indy]") {
  ResetFixture f;
  f.LoadInstruction({0x73, kSrOffset});

  f.cpu.Reset();
  SetupPointer(f);
  f.ModifyRegs([](auto& r) {
    r.A = 0x0001;
    r.P.C = false;
  });
  f.wram.WriteRegister(kEffectiveAddr, 0x05, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 54);

  REQUIRE(r.completed_cycles == 54);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x06);
}

TEST_CASE("CMP (sr,S),Y 8-bit compares via stack indirect", "[unit][opcode][cpu][sr-indy]") {
  ResetFixture f;
  f.LoadInstruction({0xD3, kSrOffset});

  f.cpu.Reset();
  SetupPointer(f);
  f.ModifyRegs([](auto& r) { r.A = 0x0042; });
  f.wram.WriteRegister(kEffectiveAddr, 0x42, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 54);

  REQUIRE(r.completed_cycles == 54);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
}
