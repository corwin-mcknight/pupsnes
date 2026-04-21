#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/cartridge.h"
#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"
#include "pupsnes/hw/wram.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)

namespace {

// Fixture duplicated from cpu_tests.cpp / opcode_alu_abs_tests.cpp (LOW-1 in
// 01-02-PLAN: acknowledged expedient; shared-fixture refactor deferred).
struct ResetFixture {
  SNES snes;
  Cartridge& cartridge;
  WRAM& wram;
  CPU& cpu;
  std::array<uint8_t, Cartridge::kLoROMWindowSize> rom{};

  ResetFixture() : cartridge(snes.GetCartridge()), wram(snes.GetWram()), cpu(snes.GetCpu()) {
    rom.fill(0xEA);
    SetResetVector(0x8000);
    SyncCartridge();
  }

  void SetResetVector(uint16_t address) {
    rom[0x7FFCU] = static_cast<uint8_t>(address & 0x00FFU);
    rom[0x7FFDU] = static_cast<uint8_t>(address >> 8U);
  }

  void SetRomByte(std::size_t offset, uint8_t value) { rom[offset] = value; }

  void SyncCartridge() { snes.LoadLoRom(rom); }
};

void SetAccumulator16(CPU& cpu, uint16_t value) {
  auto regs = cpu.GetRegs();
  regs.P.E = false;
  regs.P.M = false;
  regs.A = value;
  cpu.SetRegs(regs);
}

}  // namespace

// ============================================================================
// ALU dp,X 8-bit — one per non-BIT mnemonic (6 mnemonics × 8-bit dp,X).
// Formula 5-m+w at m=1,w=0 = 4 cycles (Bruce Clark §6.1.1.1).
// DP=0, offset=$20, X=$05 → effective address bank0:$0025.
// ============================================================================

TEST_CASE("ADC direct page indexed X 8-bit adds (DP+offset+X)", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x75);
  f.SetRomByte(0x0001U, 0x20);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0x03);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0010;
  regs.X = 0x0005;
  regs.P.C = false;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x13);
}

TEST_CASE("SBC direct page indexed X 8-bit subtracts with carry-in", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xF5);
  f.SetRomByte(0x0001U, 0x20);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0x01);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0010;
  regs.X = 0x0005;
  regs.P.C = true;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x0F);
  REQUIRE(f.cpu.GetRegs().P.C == true);
}

TEST_CASE("AND direct page indexed X 8-bit masks with memory", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x35);
  f.SetRomByte(0x0001U, 0x20);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0xF0);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x00FF;
  regs.X = 0x0005;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xF0);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("ORA direct page indexed X 8-bit combines with memory", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x15);
  f.SetRomByte(0x0001U, 0x20);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0x0F);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x00F0;
  regs.X = 0x0005;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xFF);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("EOR direct page indexed X 8-bit sets Z when equal", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x55);
  f.SetRomByte(0x0001U, 0x20);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0xFF);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x00FF;
  regs.X = 0x0005;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x00);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

TEST_CASE("CMP direct page indexed X 8-bit equal sets Z and C", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xD5);
  f.SetRomByte(0x0001U, 0x20);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0x10);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0010;
  regs.X = 0x0005;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(38);

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
  f.SetRomByte(0x0000U, 0x75);
  f.SetRomByte(0x0001U, 0x20);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0x34);
  f.wram.WriteRegister(0x0026, 0x12);
  SetAccumulator16(f.cpu, 0x1000);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0005;
  regs.P.C = false;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0x2234);
}

TEST_CASE("SBC direct page indexed X 16-bit", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xF5);
  f.SetRomByte(0x0001U, 0x20);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0x01);
  f.wram.WriteRegister(0x0026, 0x00);
  SetAccumulator16(f.cpu, 0x0002);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0005;
  regs.P.C = true;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0x0001);
  REQUIRE(f.cpu.GetRegs().P.C == true);
}

TEST_CASE("AND direct page indexed X 16-bit", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x35);
  f.SetRomByte(0x0001U, 0x20);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0x00);
  f.wram.WriteRegister(0x0026, 0xFF);
  SetAccumulator16(f.cpu, 0xFFFF);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0005;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0xFF00);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("ORA direct page indexed X 16-bit", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x15);
  f.SetRomByte(0x0001U, 0x20);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0xFF);
  f.wram.WriteRegister(0x0026, 0x00);
  SetAccumulator16(f.cpu, 0x00FF);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0005;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0x00FF);
  // N=0 (bit 15 of 0x00FF is 0), Z=0
  REQUIRE(f.cpu.GetRegs().P.N == false);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("EOR direct page indexed X 16-bit sets Z", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x55);
  f.SetRomByte(0x0001U, 0x20);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0xFF);
  f.wram.WriteRegister(0x0026, 0xFF);
  SetAccumulator16(f.cpu, 0xFFFF);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0005;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0x0000);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

TEST_CASE("CMP direct page indexed X 16-bit equal", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xD5);
  f.SetRomByte(0x0001U, 0x20);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0x34);
  f.wram.WriteRegister(0x0026, 0x12);
  SetAccumulator16(f.cpu, 0x1234);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0005;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(46);

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
  f.SetRomByte(0x0000U, 0x75);
  f.SetRomByte(0x0001U, 0x10);
  f.SyncCartridge();

  f.cpu.Reset();
  auto regs = f.cpu.GetRegs();
  regs.DP = 0x0123;
  regs.X = 0x0001;
  regs.A = 0x0001;
  regs.P.C = false;
  f.cpu.SetRegs(regs);
  f.wram.WriteRegister(0x0134, 0x02);

  TickResult r = f.cpu.Tick(44);

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
  f.SetRomByte(0x0000U, 0x34);
  f.SetRomByte(0x0001U, 0x20);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0xC0);  // bits 7 and 6 set
  auto regs = f.cpu.GetRegs();
  regs.A = 0x003F;  // A AND $C0 == 0 → Z=1
  regs.X = 0x0005;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(38);  // m=1,w=0: 5-m+w = 4

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(f.cpu.GetRegs().P.N == true);  // bit 7 of $C0
  REQUIRE(f.cpu.GetRegs().P.V == true);  // bit 6 of $C0
  REQUIRE(f.cpu.GetRegs().P.Z == true);  // A AND $C0 == 0
  // A unchanged by BIT
  REQUIRE(f.cpu.GetRegs().A == 0x003F);
}

TEST_CASE("BIT direct page indexed X 16-bit reads N V from bit 15 and 14", "[unit][opcode][cpu][dpx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x34);
  f.SetRomByte(0x0001U, 0x20);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0025, 0x00);
  f.wram.WriteRegister(0x0026, 0xC0);  // 16-bit operand = $C000 (bits 15 and 14 set)
  SetAccumulator16(f.cpu, 0x3FFF);     // A AND $C000 == 0 → Z=1
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0005;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(46);  // m=0,w=0: 5-m+w = 5

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().P.N == true);  // bit 15 of $C000
  REQUIRE(f.cpu.GetRegs().P.V == true);  // bit 14 of $C000
  REQUIRE(f.cpu.GetRegs().P.Z == true);  // A AND $C000 == 0
  // A unchanged by BIT
  REQUIRE(f.cpu.GetRegs().A == 0x3FFF);
}
