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

// Fixture duplicated from cpu_tests.cpp (LOW-1 in 01-02-PLAN: acknowledged
// expedient; shared-fixture refactor deferred to a later maintenance phase).
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

void SetIndex16X(CPU& cpu, uint16_t value) {
  auto regs = cpu.GetRegs();
  regs.P.E = false;
  regs.P.X = false;
  regs.X = value;
  cpu.SetRegs(regs);
}

void SetIndex16Y(CPU& cpu, uint16_t value) {
  auto regs = cpu.GetRegs();
  regs.P.E = false;
  regs.P.X = false;
  regs.Y = value;
  cpu.SetRegs(regs);
}

void SetDataBank(CPU& cpu, uint8_t dbr) {
  auto regs = cpu.GetRegs();
  regs.DBR = dbr;
  cpu.SetRegs(regs);
}

}  // namespace

// ============================================================================
// ALU abs 8-bit — one per mnemonic (7 mnemonics × 8-bit abs).
// Formula 5-m at m=1 = 4 cycles (Bruce Clark §6.1.1.1, §6.1.2.2).
// ============================================================================

TEST_CASE("ADC absolute 8-bit adds DBR-banked operand", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x6D);
  f.SetRomByte(0x0001U, 0x40);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x05);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x000A;
  regs.P.C = false;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x0F);
  REQUIRE(f.cpu.GetRegs().P.C == false);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
}

TEST_CASE("SBC absolute 8-bit subtracts with carry-in", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xED);
  f.SetRomByte(0x0001U, 0x50);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0050, 0x03);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0010;
  regs.P.C = true;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x0D);
  REQUIRE(f.cpu.GetRegs().P.C == true);
}

TEST_CASE("AND absolute 8-bit masks with memory", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x2D);
  f.SetRomByte(0x0001U, 0x60);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0060, 0xF0);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x00FF;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xF0);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("ORA absolute 8-bit combines with memory", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x0D);
  f.SetRomByte(0x0001U, 0x60);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0060, 0x0F);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x00F0;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xFF);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("EOR absolute 8-bit clears A to zero", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x4D);
  f.SetRomByte(0x0001U, 0x60);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0060, 0xFF);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x00FF;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x00);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

TEST_CASE("CMP absolute 8-bit equal sets Z and C", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xCD);
  f.SetRomByte(0x0001U, 0x60);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0060, 0x10);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0010;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  REQUIRE(f.cpu.GetRegs().P.N == false);
  // A must be unchanged by CMP.
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x10);
}

TEST_CASE("BIT absolute sets N and V from memory and Z from (A AND mem)", "[unit][opcode][cpu][abs]") {
  // Operand $C0 = bits 7 and 6 both set. A=$3F has no overlap with $C0, so Z=1.
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x2C);
  f.SetRomByte(0x0001U, 0x50);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0050, 0xC0);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x003F;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(f.cpu.GetRegs().P.N == true);  // bit 7 of $C0
  REQUIRE(f.cpu.GetRegs().P.V == true);  // bit 6 of $C0
  REQUIRE(f.cpu.GetRegs().P.Z == true);  // A & $C0 = 0
  // BIT must not modify A.
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x3F);
}

TEST_CASE("BIT absolute N=0 V=1 Z=0 combination", "[unit][opcode][cpu][abs]") {
  // Operand $40 = bit 6 set, bit 7 clear. A=$40 overlaps, so Z=0.
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x2C);
  f.SetRomByte(0x0001U, 0x50);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0050, 0x40);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0040;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(f.cpu.GetRegs().P.N == false);
  REQUIRE(f.cpu.GetRegs().P.V == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

// ============================================================================
// ALU abs 16-bit — one per mnemonic (6 ALU mnemonics × 16-bit abs).
// Formula 5-m at m=0 = 5 cycles (FLAG-05).
// BIT 16-bit covered by a separate case below.
// ============================================================================

TEST_CASE("ADC absolute 16-bit uses 5-cycle path", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x6D);
  f.SetRomByte(0x0001U, 0x40);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x34);
  f.wram.WriteRegister(0x0041, 0x12);
  SetAccumulator16(f.cpu, 0x1000);
  SetDataBank(f.cpu, 0x7E);
  auto regs = f.cpu.GetRegs();
  regs.P.C = false;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(f.cpu.GetRegs().A == 0x2234);
}

TEST_CASE("SBC absolute 16-bit subtracts with carry-in", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xED);
  f.SetRomByte(0x0001U, 0x40);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x01);
  f.wram.WriteRegister(0x0041, 0x00);
  SetAccumulator16(f.cpu, 0x0002);
  SetDataBank(f.cpu, 0x7E);
  auto regs = f.cpu.GetRegs();
  regs.P.C = true;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(f.cpu.GetRegs().A == 0x0001);
  REQUIRE(f.cpu.GetRegs().P.C == true);
}

TEST_CASE("AND absolute 16-bit masks wide accumulator", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x2D);
  f.SetRomByte(0x0001U, 0x40);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x00);
  f.wram.WriteRegister(0x0041, 0xFF);
  SetAccumulator16(f.cpu, 0xFFFF);
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(f.cpu.GetRegs().A == 0xFF00);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("ORA absolute 16-bit fills all bits", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x0D);
  f.SetRomByte(0x0001U, 0x40);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0xFF);
  f.wram.WriteRegister(0x0041, 0x00);
  SetAccumulator16(f.cpu, 0xFF00);
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(f.cpu.GetRegs().A == 0xFFFF);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("EOR absolute 16-bit sets Z", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x4D);
  f.SetRomByte(0x0001U, 0x40);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0xFF);
  f.wram.WriteRegister(0x0041, 0xFF);
  SetAccumulator16(f.cpu, 0xFFFF);
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(f.cpu.GetRegs().A == 0x0000);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

TEST_CASE("CMP absolute 16-bit equal", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xCD);
  f.SetRomByte(0x0001U, 0x40);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x34);
  f.wram.WriteRegister(0x0041, 0x12);
  SetAccumulator16(f.cpu, 0x1234);
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  // A must be unchanged.
  REQUIRE(f.cpu.GetRegs().A == 0x1234);
}

TEST_CASE("BIT absolute 16-bit reads N from bit 15 and V from bit 14", "[unit][opcode][cpu][abs]") {
  // Operand $C000 = bits 15 and 14 set. A=$3FFF has no overlap with $C000, so Z=1.
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x2C);
  f.SetRomByte(0x0001U, 0x40);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x00);
  f.wram.WriteRegister(0x0041, 0xC0);
  SetAccumulator16(f.cpu, 0x3FFF);
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.V == true);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().A == 0x3FFF);  // A unchanged
}

// ============================================================================
// ALU abs long 8-bit — one per mnemonic (6 ALU mnemonics × 8-bit long).
// Formula 6-m at m=1 = 5 cycles.
// ============================================================================

TEST_CASE("ADC absolute long 8-bit loads from 24-bit address", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x6F);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0x02);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0001;
  regs.P.C = false;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x03);
  REQUIRE(f.cpu.GetRegs().PC == 0x8004);
}

TEST_CASE("SBC absolute long 8-bit", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xEF);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0x01);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0010;
  regs.P.C = true;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x0F);
}

TEST_CASE("AND absolute long 8-bit", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x2F);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0xF0);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x00FF;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xF0);
}

TEST_CASE("ORA absolute long 8-bit", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x0F);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0x0F);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x00F0;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xFF);
}

TEST_CASE("EOR absolute long 8-bit clears A", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x4F);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0xFF);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x00FF;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x00);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

TEST_CASE("CMP absolute long 8-bit equal", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xCF);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0x10);
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0010;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
}

// ============================================================================
// ALU abs long 16-bit — one per mnemonic (6 ALU mnemonics × 16-bit long).
// Formula 6-m at m=0 = 6 cycles (FLAG-05).
// ============================================================================

TEST_CASE("ADC absolute long 16-bit uses 6 cycles", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x6F);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0x34);
  f.wram.WriteRegister(0x0001, 0x12);
  SetAccumulator16(f.cpu, 0x1000);
  auto regs = f.cpu.GetRegs();
  regs.P.C = false;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(6);

  REQUIRE(r.completed_cycles == 6);
  REQUIRE(f.cpu.GetRegs().A == 0x2234);
}

TEST_CASE("SBC absolute long 16-bit", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xEF);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0x01);
  f.wram.WriteRegister(0x0001, 0x00);
  SetAccumulator16(f.cpu, 0x0002);
  auto regs = f.cpu.GetRegs();
  regs.P.C = true;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(6);

  REQUIRE(r.completed_cycles == 6);
  REQUIRE(f.cpu.GetRegs().A == 0x0001);
  REQUIRE(f.cpu.GetRegs().P.C == true);
}

TEST_CASE("AND absolute long 16-bit", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x2F);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0x00);
  f.wram.WriteRegister(0x0001, 0xFF);
  SetAccumulator16(f.cpu, 0xFFFF);

  TickResult r = f.cpu.Tick(6);

  REQUIRE(r.completed_cycles == 6);
  REQUIRE(f.cpu.GetRegs().A == 0xFF00);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("ORA absolute long 16-bit", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x0F);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0xFF);
  f.wram.WriteRegister(0x0001, 0x00);
  SetAccumulator16(f.cpu, 0xFF00);

  TickResult r = f.cpu.Tick(6);

  REQUIRE(r.completed_cycles == 6);
  REQUIRE(f.cpu.GetRegs().A == 0xFFFF);
}

TEST_CASE("EOR absolute long 16-bit sets Z", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x4F);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0xFF);
  f.wram.WriteRegister(0x0001, 0xFF);
  SetAccumulator16(f.cpu, 0xFFFF);

  TickResult r = f.cpu.Tick(6);

  REQUIRE(r.completed_cycles == 6);
  REQUIRE(f.cpu.GetRegs().A == 0x0000);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

TEST_CASE("CMP absolute long 16-bit equal", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xCF);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0x34);
  f.wram.WriteRegister(0x0001, 0x12);
  SetAccumulator16(f.cpu, 0x1234);

  TickResult r = f.cpu.Tick(6);

  REQUIRE(r.completed_cycles == 6);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  REQUIRE(f.cpu.GetRegs().A == 0x1234);  // unchanged
}

// ============================================================================
// Loads and Compare: LDA abs, LDA long, LDX abs, LDY abs, CPX abs, CPY abs.
// ============================================================================

TEST_CASE("LDA absolute loads 8-bit value from DBR-banked address", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xAD);
  f.SetRomByte(0x0001U, 0x40);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x42);
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x42);
}

TEST_CASE("LDA absolute long loads from 24-bit address", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xAF);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0000, 0xBB);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xBB);
}

TEST_CASE("LDX absolute 8-bit loads X low byte", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xAE);
  f.SetRomByte(0x0001U, 0x40);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x55);
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().X) == 0x55);
}

TEST_CASE("LDX absolute 16-bit uses 5-cycle path", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xAE);
  f.SetRomByte(0x0001U, 0x40);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x34);
  f.wram.WriteRegister(0x0041, 0x12);
  SetIndex16X(f.cpu, 0x0000);
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(f.cpu.GetRegs().X == 0x1234);
}

TEST_CASE("LDY absolute 8-bit loads Y low byte", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xAC);
  f.SetRomByte(0x0001U, 0x40);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x33);
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().Y) == 0x33);
}

TEST_CASE("LDY absolute 16-bit uses 5-cycle path", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xAC);
  f.SetRomByte(0x0001U, 0x40);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0xCD);
  f.wram.WriteRegister(0x0041, 0xAB);
  SetIndex16Y(f.cpu, 0x0000);
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(f.cpu.GetRegs().Y == 0xABCD);
}

TEST_CASE("CPX absolute equal sets Z and C", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xEC);
  f.SetRomByte(0x0001U, 0x40);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x10);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0010;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  // X unchanged.
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().X) == 0x10);
}

TEST_CASE("CPY absolute Y > operand sets C but not Z", "[unit][opcode][cpu][abs]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xCC);
  f.SetRomByte(0x0001U, 0x40);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x10);
  auto regs = f.cpu.GetRegs();
  regs.Y = 0x0020;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}
