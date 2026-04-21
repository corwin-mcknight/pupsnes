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

// Fixture duplicated from opcode_alu_abs_tests.cpp (shared-fixture refactor
// deferred).
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
// ALU long,X 8-bit — one TEST_CASE per mnemonic (ADC/SBC/AND/ORA/EOR/CMP).
// Cycle formula 6-m at m=1 = 6 cycles (Bruce Clark §6.1.1.1).
// Base operand $7E:0000, X=$10 → effective address $7E:0010.
// ============================================================================

TEST_CASE("ADC long,X 8-bit adds indexed operand", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x7F);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0010, 0x05);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0010;
  regs.A = 0x0001;
  regs.P.C = false;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x06);
  REQUIRE(f.cpu.GetRegs().PC == 0x8004);
}

TEST_CASE("SBC long,X 8-bit subtracts indexed operand", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xFF);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0010, 0x01);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0010;
  regs.A = 0x0010;
  regs.P.C = true;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x0F);
}

TEST_CASE("AND long,X 8-bit", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x3F);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0010, 0xF0);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0010;
  regs.A = 0x00FF;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xF0);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("ORA long,X 8-bit", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x1F);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0010, 0x0F);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0010;
  regs.A = 0x00F0;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xFF);
}

TEST_CASE("EOR long,X 8-bit clears to zero", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x5F);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0010, 0xFF);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0010;
  regs.A = 0x00FF;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x00);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

TEST_CASE("CMP long,X 8-bit equal sets Z and C", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xDF);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0010, 0x42);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0010;
  regs.A = 0x0042;
  f.cpu.SetRegs(regs);

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
  f.SetRomByte(0x0000U, 0xBF);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0020, 0x81);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0020;
  regs.A = 0x0000;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x81);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("STA long,X 8-bit writes indexed operand", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x9F);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0030, 0x00);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0030;
  regs.A = 0x0055;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.wram.ReadRegister(0x0030) == 0x55);
}

// ============================================================================
// 16-bit M path — formula 6-m at m=0 = 7 cycles.
// ============================================================================

TEST_CASE("ADC long,X 16-bit uses 7 cycles", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x7F);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0x1000);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0010;
  regs.P.C = false;
  f.cpu.SetRegs(regs);
  f.wram.WriteRegister(0x0010, 0x34);
  f.wram.WriteRegister(0x0011, 0x12);

  TickResult r = f.cpu.Tick(54);

  REQUIRE(r.completed_cycles == 54);
  REQUIRE(f.cpu.GetRegs().A == 0x2234);
}

TEST_CASE("LDA long,X 16-bit loads wide operand", "[unit][opcode][cpu][longx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xBF);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0x0000);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0010;
  f.cpu.SetRegs(regs);
  f.wram.WriteRegister(0x0010, 0xCD);
  f.wram.WriteRegister(0x0011, 0xAB);

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
  f.SetRomByte(0x0000U, 0xBF);
  f.SetRomByte(0x0001U, 0xFF);
  f.SetRomByte(0x0002U, 0xFF);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x10000U, 0x77);  // $7F:0000 in WRAM linear space.
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0001;
  regs.A = 0x0000;
  f.cpu.SetRegs(regs);

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
  f.SetRomByte(0x0000U, 0xBF);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x33);
  auto regs = f.cpu.GetRegs();
  regs.DBR = 0x12;  // bogus DBR — long,X must still use operand bank.
  regs.X = 0x0040;
  regs.A = 0x0000;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x33);
}
