#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstdint>
#include <initializer_list>

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
// ALU (dp) indirect — 8-bit accumulator, DP=0 (no DL penalty).
// Cycle formula 6-m+w at m=1, w=0 = 5 cycles total = 40 master cycles
// (5 bus accesses × 8). One TEST_CASE per ALU mnemonic. Pointer at bank-0 DP
// $0010 → DBR:$1234.
// ============================================================================

namespace {

constexpr uint8_t kDpOffset = 0x10;
constexpr uint16_t kPtrAddr = 0x0010;
constexpr uint16_t kEffectiveAddr = 0x1234;
constexpr uint8_t kDbr = 0x7E;

void SetupIndirectPointer(ResetFixture& f) {
  f.ModifyRegs([](auto& r) { r.DBR = kDbr; });
  f.wram.WriteRegister(kPtrAddr, 0x34, 0);
  f.wram.WriteRegister(kPtrAddr + 1, 0x12, 0);
}

}  // namespace

TEST_CASE("ADC (dp) 8-bit adds through pointer", "[unit][opcode][cpu][indirect]") {
  ResetFixture f;
  f.LoadInstruction({0x72, kDpOffset});

  f.cpu.Reset();
  SetupIndirectPointer(f);
  f.ModifyRegs([](auto& r) {
    r.A = 0x0001;
    r.P.C = false;
  });
  f.wram.WriteRegister(kEffectiveAddr, 0x05, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x06);
  REQUIRE(f.cpu.GetRegs().P.C == false);
}

TEST_CASE("AND (dp) 8-bit masks through pointer", "[unit][opcode][cpu][indirect]") {
  ResetFixture f;
  f.LoadInstruction({0x32, kDpOffset});

  f.cpu.Reset();
  SetupIndirectPointer(f);
  f.ModifyRegs([](auto& r) { r.A = 0x00FF; });
  f.wram.WriteRegister(kEffectiveAddr, 0xF0, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xF0);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("CMP (dp) 8-bit equal sets Z and C", "[unit][opcode][cpu][indirect]") {
  ResetFixture f;
  f.LoadInstruction({0xD2, kDpOffset});

  f.cpu.Reset();
  SetupIndirectPointer(f);
  f.ModifyRegs([](auto& r) { r.A = 0x0042; });
  f.wram.WriteRegister(kEffectiveAddr, 0x42, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}

// ============================================================================
// ALU (dp) indirect — 16-bit accumulator (M=0). Cycle formula 6-m+w at m=0,
// w=0 = 6 cycles total = 48 master cycles. One spot-check on ADC; the other
// mnemonics share the AluFromAddr 16-bit lowering, so an ADC + an ORA cover it.
// ============================================================================

TEST_CASE("ADC (dp) 16-bit adds 16-bit value", "[unit][opcode][cpu][indirect]") {
  ResetFixture f;
  f.LoadInstruction({0x72, kDpOffset});

  f.cpu.Reset();
  SetupIndirectPointer(f);
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.P.M = false;
    r.A = 0x0100;
    r.P.C = false;
    r.DBR = kDbr;
  });
  f.wram.WriteRegister(kEffectiveAddr, 0x01, 0);
  f.wram.WriteRegister(kEffectiveAddr + 1, 0x02, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(f.cpu.GetRegs().A == 0x0301);
  REQUIRE(f.cpu.GetRegs().P.C == false);
}

// ============================================================================
// ALU (dp) — DL-nonzero penalty (DP low byte != 0) adds one internal (+6
// master) cycle. 5 bus × 8 + 1 internal × 6 = 46 master. Stop on instruction
// retirement so a subsequent opcode cannot absorb a missing penalty.
// ============================================================================

TEST_CASE("ADC (dp) DL-nonzero pays penalty cycle", "[unit][opcode][cpu][indirect]") {
  ResetFixture f;
  f.LoadInstruction({0x72, kDpOffset});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.DP = 0x0080;  // DL=0x80 → +1 internal cycle
    r.DBR = kDbr;
    r.A = 0x0001;
    r.P.C = false;
  });
  // Pointer now at $0090, $0091.
  f.wram.WriteRegister(0x0090, 0x34, 0);
  f.wram.WriteRegister(0x0091, 0x12, 0);
  f.wram.WriteRegister(kEffectiveAddr, 0x05, 0);

  f.cpu.MutableDebuggerContract().step_target = 1;
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(r.reason == TickStopReason::kRetiredStepTarget);
  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x06);
}

TEST_CASE("Every DP-indirect addressing family charges its penalty at retirement", "[unit][opcode][cpu][indirect]") {
  struct Mode {
    uint8_t opcode;
    bool long_pointer;
    TimeMasterDeltaT narrow_cycles;
  };
  constexpr std::array kModes{
      Mode{0xB2, false, 40},  // LDA (dp)
      Mode{0xA7, true, 48},   // LDA [dp]
      Mode{0xA1, false, 46},  // LDA (dp,X): internal index-add cycle
      Mode{0xB1, false, 46},  // LDA (dp),Y: internal index-add cycle
      Mode{0xB7, true, 48},   // LDA [dp],Y: index add folded into bank fetch
  };
  for (const Mode mode : kModes) {
    for (const bool wide : {false, true}) {
      for (const uint16_t dp : {uint16_t{0}, uint16_t{0x80}}) {
        CAPTURE(mode.opcode, wide, dp);
        ResetFixture f;
        f.LoadInstruction({mode.opcode, kDpOffset});
        f.cpu.Reset();
        f.ModifyRegs([&](auto& regs) {
          regs.P.E = false;
          regs.P.M = !wide;
          regs.DP = dp;
          regs.DBR = kDbr;
        });
        const uint32_t pointer = static_cast<uint32_t>(dp) + kDpOffset;
        f.wram.WriteRegister(pointer, 0x34, 0);
        f.wram.WriteRegister(pointer + 1U, 0x12, 0);
        if (mode.long_pointer) f.wram.WriteRegister(pointer + 2U, kDbr, 0);
        f.wram.WriteRegister(kEffectiveAddr, 0x42, 0);
        f.wram.WriteRegister(kEffectiveAddr + 1U, 0xAB, 0);

        f.cpu.MutableDebuggerContract().step_target = 1;
        const TickResult result = f.cpu.TickToTarget(100);
        const TimeMasterDeltaT expected = mode.narrow_cycles + (wide ? 8U : 0U) + (dp != 0U ? 6U : 0U);
        REQUIRE(result.reason == TickStopReason::kRetiredStepTarget);
        REQUIRE(result.completed_cycles == expected);
        REQUIRE(f.snes.GetMasterTime() == expected);
        REQUIRE(f.cpu.GetRetiredInstructionCount() == 1U);
        REQUIRE(f.cpu.GetRegs().PC == 0x8002U);
        REQUIRE(f.cpu.GetRegs().A == (wide ? 0xAB42U : 0x42U));
      }
    }
  }
}

// ============================================================================
// ALU [dp] indirect long — 8-bit. Pointer is 24-bit; bank comes from memory,
// not DBR. Cycle formula 7-m+w at m=1, w=0 = 6 cycles = 48 master cycles
// (6 bus accesses × 8). One TEST_CASE per ALU mnemonic.
// ============================================================================

namespace {

constexpr uint8_t kLongDpOffset = 0x40;
constexpr uint16_t kLongPtrAddr = 0x0040;
constexpr uint8_t kLongEffectiveBank = 0x7E;
constexpr uint16_t kLongEffectiveOffset = 0x3010;

void SetupIndirectLongPointer(ResetFixture& f) {
  // 24-bit pointer at $0040 = $7E:3010
  f.wram.WriteRegister(kLongPtrAddr, 0x10, 0);
  f.wram.WriteRegister(kLongPtrAddr + 1, 0x30, 0);
  f.wram.WriteRegister(kLongPtrAddr + 2, kLongEffectiveBank, 0);
}

}  // namespace

TEST_CASE("ADC [dp] 8-bit adds through 24-bit pointer", "[unit][opcode][cpu][indirect]") {
  ResetFixture f;
  f.LoadInstruction({0x67, kLongDpOffset});

  f.cpu.Reset();
  SetupIndirectLongPointer(f);
  f.ModifyRegs([](auto& r) {
    r.A = 0x0001;
    r.P.C = false;
  });
  f.wram.WriteRegister(kLongEffectiveOffset, 0x10, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x11);
}

TEST_CASE("AND [dp] 8-bit masks through 24-bit pointer", "[unit][opcode][cpu][indirect]") {
  ResetFixture f;
  f.LoadInstruction({0x27, kLongDpOffset});

  f.cpu.Reset();
  SetupIndirectLongPointer(f);
  f.ModifyRegs([](auto& r) { r.A = 0x00AA; });
  f.wram.WriteRegister(kLongEffectiveOffset, 0x0F, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x0A);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}

TEST_CASE("CMP [dp] 8-bit less-than clears C", "[unit][opcode][cpu][indirect]") {
  ResetFixture f;
  f.LoadInstruction({0xC7, kLongDpOffset});

  f.cpu.Reset();
  SetupIndirectLongPointer(f);
  f.ModifyRegs([](auto& r) { r.A = 0x0001; });
  f.wram.WriteRegister(kLongEffectiveOffset, 0x10, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(f.cpu.GetRegs().P.C == false);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

// ============================================================================
// ALU [dp] — 16-bit spot-check. Cycle formula 7-m+w at m=0, w=0 = 7 cycles =
// 56 master cycles (7 bus × 8).
// ============================================================================

TEST_CASE("ADC [dp] 16-bit adds 16-bit value", "[unit][opcode][cpu][indirect]") {
  ResetFixture f;
  f.LoadInstruction({0x67, kLongDpOffset});

  f.cpu.Reset();
  SetupIndirectLongPointer(f);
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.P.M = false;
    r.A = 0x0001;
    r.P.C = false;
  });
  f.wram.WriteRegister(kLongEffectiveOffset, 0x34, 0);
  f.wram.WriteRegister(kLongEffectiveOffset + 1, 0x12, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 56);

  REQUIRE(r.completed_cycles == 56);
  REQUIRE(f.cpu.GetRegs().A == 0x1235);
}
