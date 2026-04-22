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

void SetIndex16X(CPU& cpu, uint16_t value) {
  auto regs = cpu.GetRegs();
  regs.P.E = false;
  regs.P.X = false;
  regs.X = value;
  cpu.SetRegs(regs);
}

}  // namespace

// ============================================================================
// ALU abs,X 8-bit — one TEST_CASE per mnemonic (ADC/SBC/AND/ORA/EOR/CMP).
// Cycle formula 5-m (our always-pay-penalty lowering) at m=1 = 5 cycles
// (Bruce Clark §6.1.1.1, "4-m+x+x*p" with x=1/p=1 fast-path collapsed to the
// unconditional penalty — we overcount p=0 cases by 1 cycle deliberately).
// Base operand $0100 with DBR=$7E and X=$10 → effective address $7E:0110.
// ============================================================================

TEST_CASE("ADC abs,X 8-bit adds DBR-banked indexed operand", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x7D);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x01);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0110, 0x05);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0010;
  regs.A = 0x0001;
  regs.P.C = false;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x06);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
}

TEST_CASE("SBC abs,X 8-bit subtracts indexed operand", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xFD);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x01);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0110, 0x01);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0010;
  regs.A = 0x0010;
  regs.P.C = true;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x0F);
}

TEST_CASE("AND abs,X 8-bit", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x3D);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x01);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0110, 0xF0);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0010;
  regs.A = 0x00FF;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xF0);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("ORA abs,X 8-bit", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x1D);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x01);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0110, 0x0F);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0010;
  regs.A = 0x00F0;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xFF);
}

TEST_CASE("EOR abs,X 8-bit clears to zero", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x5D);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x01);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0110, 0xFF);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0010;
  regs.A = 0x00FF;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x00);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

TEST_CASE("CMP abs,X 8-bit equal sets Z and C", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xDD);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x01);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0110, 0x42);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0010;
  regs.A = 0x0042;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  // A unchanged.
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x42);
}

// ============================================================================
// LDA/LDY abs,X — exercise the A-width and X-width load paths.
// LDA formula 5-m (our lowering); LDY formula 5-x.
// ============================================================================

TEST_CASE("LDA abs,X 8-bit loads indexed operand", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xBD);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x01);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0120, 0x81);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0020;
  regs.A = 0x0000;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x81);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("LDY abs,X 8-bit loads indexed operand into Y", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xBC);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x01);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0130, 0x42);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0030;
  regs.Y = 0x0000;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().Y) == 0x42);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}

// ============================================================================
// 16-bit M path — formula 5-m at m=0 = 6 cycles = 46 master cycles.
// ============================================================================

TEST_CASE("ADC abs,X 16-bit adds 16-bit operand", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x7D);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x01);
  f.SyncCartridge();

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0x1000);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0010;
  regs.P.C = false;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);
  f.wram.WriteRegister(0x0110, 0x34);
  f.wram.WriteRegister(0x0111, 0x12);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0x2234);
}

TEST_CASE("LDA abs,X 16-bit loads wide operand", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xBD);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x01);
  f.SyncCartridge();

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0x0000);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0010;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);
  f.wram.WriteRegister(0x0110, 0xCD);
  f.wram.WriteRegister(0x0111, 0xAB);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0xABCD);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

// ============================================================================
// 16-bit X path — LDY abs,X with X=0 (16-bit indices): formula 5-x at x=0 = 5,
// loading 16 bits into Y takes one extra cycle (6 cycles total = 46 masters).
// ============================================================================

TEST_CASE("LDY abs,X 16-bit loads wide operand into Y", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xBC);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x01);
  f.SyncCartridge();

  f.cpu.Reset();
  SetIndex16X(f.cpu, 0x0010);
  auto regs = f.cpu.GetRegs();
  regs.Y = 0x0000;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);
  f.wram.WriteRegister(0x0110, 0x34);
  f.wram.WriteRegister(0x0111, 0x12);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().Y == 0x1234);
}

// ============================================================================
// Bank carry on index add: base operand $FFF0 with DBR=$7E and X=$20 →
// effective $7F:0010 (low-16 overflow carries into bank byte).
// ============================================================================

TEST_CASE("LDA abs,X carries into bank byte on index overflow", "[unit][opcode][cpu][absx]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xBD);
  f.SetRomByte(0x0001U, 0xF0);
  f.SetRomByte(0x0002U, 0xFF);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x10010U, 0x77);  // $7F:0010 in WRAM linear space.
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0020;
  regs.A = 0x0000;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x77);
}
