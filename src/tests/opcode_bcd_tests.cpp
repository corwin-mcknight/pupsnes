#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <initializer_list>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)

namespace {

// Minimal ROM device mirroring the TestROM fixture in cpu_tests.cpp — provides
// a small addressable region mapped at bank 0x00, pages 0x80/0x81 so that
// immediate-operand opcodes can be executed without pulling in Cartridge/WRAM.
// Fixture duplicated (not shared) per the same rationale recorded in
// opcode_alu_abs_tests.cpp: shared-fixture refactor is a deferred maintenance
// task, not a Phase 1 concern.
class TestROM : public Device {
 public:
  static constexpr std::size_t kSize = 512;
  std::array<uint8_t, kSize> mem{};

  explicit TestROM(SNES* snes) : Device(snes) {}

  TickResult Tick(TimeMasterDeltaT budget) override { return {budget, TickStopReason::kBudgetExhausted}; }
  void OnEvent(const SchedulerEvent&) override {}

  uint8_t ReadRegister(uint32_t offset) override { return mem[offset % kSize]; }
  void WriteRegister(uint32_t offset, uint8_t data) override { mem[offset % kSize] = data; }
};

struct TestFixture {
  SNES snes;
  TestROM rom{&snes};
  CPU cpu{&snes};

  TestFixture() {
    snes.system_bus->MapPage({0x00, 0x80, rom.GetDeviceId(), 0x000, PageDeviceKind::kMemory, 8});
    snes.system_bus->MapPage({0x00, 0x81, rom.GetDeviceId(), 0x100, PageDeviceKind::kMemory, 8});

    auto r = cpu.GetRegs();
    r.PC = 0x8000;
    cpu.SetRegs(r);
  }

  void LoadAt(uint16_t offset, std::initializer_list<uint8_t> bytes) {
    std::size_t i = offset & 0x1FFu;
    for (uint8_t b : bytes) {
      rom.mem[i++ & 0x1FFu] = b;
    }
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// BCD (decimal mode) ADC / SBC — D=1 path
//
// Locked fixture SC #4 + the D-14 nibble-boundary sweep from 01-RESEARCH.md
// §Research Priority 5. Every test asserts all four flags (N, V, Z, C)
// explicitly per MEDIUM-4 / HIGH-5 from 01-REVIEWS.md.
//
// Bruce Clark's 65C816 BCD reference: docs/external/6502opcodes.md §6.1.1.1.
// ---------------------------------------------------------------------------

TEST_CASE("BCD SBC 16-bit locked: A=$0001 - #$2003 D=1 C=1 -> A=$7998", "[unit][opcode][cpu][bcd]") {
  // Locked fixture SC #4 (ROADMAP). m=0, D=1, C=1.
  // ~$2003 & $FFFF = $DFFC; BcdAdd16(0x0001, 0xDFFC, true) -> $7998.
  // bin_sum = 0x0001 + 0xDFFC + 1 = 0xDFFE (no carry out of 16 bits).
  // V = ((0x0001 ^ 0xDFFE) & (0xDFFC ^ 0xDFFE) & 0x8000) = 0 -> V=false.
  TestFixture f;
  f.LoadAt(0, {0xE9, 0x03, 0x20});  // SBC #$2003 (16-bit immediate)
  auto regs = f.cpu.GetRegs();
  regs.P.E = false;
  regs.P.M = false;  // 16-bit accumulator
  regs.A = 0x0001;
  regs.P.D = true;
  regs.P.C = true;
  f.cpu.SetRegs(regs);
  TickResult r = f.cpu.Tick(3);
  REQUIRE(r.completed_cycles == 3);
  REQUIRE(f.cpu.GetRegs().A == 0x7998);
  REQUIRE(f.cpu.GetRegs().P.N == false);
  REQUIRE(f.cpu.GetRegs().P.V == false);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
  REQUIRE(f.cpu.GetRegs().P.C == false);
}

TEST_CASE("BCD ADC 8-bit: $09 + $01 = $10 (unit-nibble carry)", "[unit][opcode][cpu][bcd]") {
  TestFixture f;
  f.LoadAt(0, {0x69, 0x01});  // ADC #$01
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0009;
  regs.P.D = true;
  regs.P.C = false;
  f.cpu.SetRegs(regs);
  TickResult r = f.cpu.Tick(2);
  REQUIRE(r.completed_cycles == 2);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x10);
  REQUIRE(f.cpu.GetRegs().P.N == false);
  REQUIRE(f.cpu.GetRegs().P.V == false);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
  REQUIRE(f.cpu.GetRegs().P.C == false);
}

TEST_CASE("BCD ADC 8-bit: $50 + $50 = $00 C=1 V=1 N=0", "[unit][opcode][cpu][bcd]") {
  // bin_sum = $A0; low nibble 0 <= 9 (no low fixup).
  // adj=$A0 > $99 -> +$60 = $100 -> result=$00, C=1.
  // V: ((~($50^$50)) & ($50^$A0)) & $80 = ($FF & $F0) & $80 = $80 -> V=true.
  // N: result $00 -> N=false.
  TestFixture f;
  f.LoadAt(0, {0x69, 0x50});  // ADC #$50
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0050;
  regs.P.D = true;
  regs.P.C = false;
  f.cpu.SetRegs(regs);
  TickResult r = f.cpu.Tick(2);
  REQUIRE(r.completed_cycles == 2);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x00);
  REQUIRE(f.cpu.GetRegs().P.N == false);
  REQUIRE(f.cpu.GetRegs().P.V == true);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
}

TEST_CASE("BCD ADC 8-bit: $40 + $40 = $80 N=1 V=1", "[unit][opcode][cpu][bcd]") {
  // bin_sum = $80; low nibble 0 (no fixup); adj=$80 <= $99 (no high fixup).
  // result = $80 -> N=true, Z=false, C=false.
  // V: ((~($40^$40)) & ($40^$80)) & $80 = ($FF & $C0) & $80 = $80 -> V=true.
  TestFixture f;
  f.LoadAt(0, {0x69, 0x40});  // ADC #$40
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0040;
  regs.P.D = true;
  regs.P.C = false;
  f.cpu.SetRegs(regs);
  TickResult r = f.cpu.Tick(2);
  REQUIRE(r.completed_cycles == 2);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x80);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.V == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
  REQUIRE(f.cpu.GetRegs().P.C == false);
}

TEST_CASE("BCD ADC 8-bit: $99 + $01 = $00 C=1 (full rollover)", "[unit][opcode][cpu][bcd]") {
  TestFixture f;
  f.LoadAt(0, {0x69, 0x01});  // ADC #$01
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0099;
  regs.P.D = true;
  regs.P.C = false;
  f.cpu.SetRegs(regs);
  TickResult r = f.cpu.Tick(2);
  REQUIRE(r.completed_cycles == 2);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x00);
  REQUIRE(f.cpu.GetRegs().P.N == false);
  REQUIRE(f.cpu.GetRegs().P.V == false);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
}

TEST_CASE("BCD ADC 16-bit: $0999 + $0001 = $1000 (nibble propagation)", "[unit][opcode][cpu][bcd]") {
  TestFixture f;
  f.LoadAt(0, {0x69, 0x01, 0x00});  // ADC #$0001 (little-endian)
  auto regs = f.cpu.GetRegs();
  regs.P.E = false;
  regs.P.M = false;  // 16-bit accumulator
  regs.A = 0x0999;
  regs.P.D = true;
  regs.P.C = false;
  f.cpu.SetRegs(regs);
  TickResult r = f.cpu.Tick(3);
  REQUIRE(r.completed_cycles == 3);
  REQUIRE(f.cpu.GetRegs().A == 0x1000);
  REQUIRE(f.cpu.GetRegs().P.N == false);
  REQUIRE(f.cpu.GetRegs().P.V == false);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
  REQUIRE(f.cpu.GetRegs().P.C == false);
}

TEST_CASE("BCD SBC 8-bit with borrow: $50 - $01 C=0 = $48", "[unit][opcode][cpu][bcd]") {
  // SBC uses ones-complement. C=0 means borrow-in.
  // bcd_add($50, ~$01 & $FF = $FE, false) -> ...
  TestFixture f;
  f.LoadAt(0, {0xE9, 0x01});  // SBC #$01
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0050;
  regs.P.D = true;
  regs.P.C = false;  // borrow-in
  f.cpu.SetRegs(regs);
  TickResult r = f.cpu.Tick(2);
  REQUIRE(r.completed_cycles == 2);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x48);
  REQUIRE(f.cpu.GetRegs().P.N == false);
  REQUIRE(f.cpu.GetRegs().P.V == false);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
  REQUIRE(f.cpu.GetRegs().P.C == true);  // C=1 = no borrow out
}

TEST_CASE("BCD ADC invalid digit $0A + $01 = $11", "[unit][opcode][cpu][bcd]") {
  // Per Bruce Clark §6.1.1.1 invalid-digit behavior: $0A + $01 bin_sum=$0B,
  // low nibble $B > 9 -> +6 = $11. adj=$11 <= $99 -> C=0.
  TestFixture f;
  f.LoadAt(0, {0x69, 0x01});  // ADC #$01
  auto regs = f.cpu.GetRegs();
  regs.A = 0x000A;
  regs.P.D = true;
  regs.P.C = false;
  f.cpu.SetRegs(regs);
  TickResult r = f.cpu.Tick(2);
  REQUIRE(r.completed_cycles == 2);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x11);
  REQUIRE(f.cpu.GetRegs().P.N == false);
  REQUIRE(f.cpu.GetRegs().P.V == false);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
  REQUIRE(f.cpu.GetRegs().P.C == false);
}

TEST_CASE("Binary ADC D=0 is unaffected by BCD changes (regression guard)", "[unit][opcode][cpu][bcd]") {
  // When D=0, $09 + $01 must produce $0A (binary), not $10 (BCD).
  TestFixture f;
  f.LoadAt(0, {0x69, 0x01});  // ADC #$01
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0009;
  regs.P.D = false;  // binary mode
  regs.P.C = false;
  f.cpu.SetRegs(regs);
  TickResult r = f.cpu.Tick(2);
  REQUIRE(r.completed_cycles == 2);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x0A);
  REQUIRE(f.cpu.GetRegs().P.N == false);
  REQUIRE(f.cpu.GetRegs().P.V == false);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
  REQUIRE(f.cpu.GetRegs().P.C == false);
}

TEST_CASE("BCD ADC 16-bit: $4000 + $4000 = $8000 V=1 N=1 (signed boundary)", "[unit][opcode][cpu][bcd]") {
  // HIGH-5 guard: V must come from the full 16-bit pre-adjustment binary sum,
  // NOT from the high-byte BcdAdd8's overflow field.
  // bin_sum = 0x8000; V = ((0x4000^0x8000) & (0x4000^0x8000) & 0x8000)
  //                     = (0xC000 & 0xC000 & 0x8000) = 0x8000 -> V=true.
  // BCD fixup: digits 4,0,0,0 — no nibble > 9 and no nibble-carry; adj=$8000
  // <= $9999 so no high-word fixup. Result = $8000. N=true (bit 15).
  TestFixture f;
  f.LoadAt(0, {0x69, 0x00, 0x40});  // ADC #$4000 (little-endian: lo=$00, hi=$40)
  auto regs = f.cpu.GetRegs();
  regs.P.E = false;
  regs.P.M = false;  // 16-bit accumulator
  regs.A = 0x4000;
  regs.P.D = true;
  regs.P.C = false;
  f.cpu.SetRegs(regs);
  TickResult r = f.cpu.Tick(3);
  REQUIRE(r.completed_cycles == 3);
  REQUIRE(f.cpu.GetRegs().A == 0x8000);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.V == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
  REQUIRE(f.cpu.GetRegs().P.C == false);
}
