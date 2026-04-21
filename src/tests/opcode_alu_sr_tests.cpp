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

// Fixture duplicated from cpu_tests.cpp / opcode_alu_dpx_tests.cpp (LOW-1 in
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
// ALU sr,S 8-bit — one TEST_CASE per ALU mnemonic (ADC/SBC/AND/ORA/EOR/CMP).
// Cycle formula 5-m at m=1 = 4 cycles (Bruce Clark §6.1.1.1). No DL penalty.
// SP=0x01F0, offset=$04 → effective address bank0:$01F4.
// ============================================================================

TEST_CASE("ADC stack-relative 8-bit adds from bank-0 SP+offset", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x63);
  f.SetRomByte(0x0001U, 0x04);
  f.SyncCartridge();

  f.cpu.Reset();
  auto regs = f.cpu.GetRegs();
  regs.SP = 0x01F0;
  regs.A = 0x0001;
  regs.P.C = false;
  f.cpu.SetRegs(regs);
  f.wram.WriteRegister(0x01F4, 0x05);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x06);
  REQUIRE(f.cpu.GetRegs().P.C == false);
}

TEST_CASE("SBC stack-relative 8-bit subtracts from stack", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xE3);
  f.SetRomByte(0x0001U, 0x04);
  f.SyncCartridge();

  f.cpu.Reset();
  auto regs = f.cpu.GetRegs();
  regs.SP = 0x01F0;
  regs.A = 0x0010;
  regs.P.C = true;
  f.cpu.SetRegs(regs);
  f.wram.WriteRegister(0x01F4, 0x01);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x0F);
  REQUIRE(f.cpu.GetRegs().P.C == true);
}

TEST_CASE("AND stack-relative 8-bit", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x23);
  f.SetRomByte(0x0001U, 0x04);
  f.SyncCartridge();

  f.cpu.Reset();
  auto regs = f.cpu.GetRegs();
  regs.SP = 0x01F0;
  regs.A = 0x00FF;
  f.cpu.SetRegs(regs);
  f.wram.WriteRegister(0x01F4, 0xF0);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xF0);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("ORA stack-relative 8-bit", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x03);
  f.SetRomByte(0x0001U, 0x04);
  f.SyncCartridge();

  f.cpu.Reset();
  auto regs = f.cpu.GetRegs();
  regs.SP = 0x01F0;
  regs.A = 0x00F0;
  f.cpu.SetRegs(regs);
  f.wram.WriteRegister(0x01F4, 0x0F);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xFF);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("EOR stack-relative 8-bit clears to zero", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x43);
  f.SetRomByte(0x0001U, 0x04);
  f.SyncCartridge();

  f.cpu.Reset();
  auto regs = f.cpu.GetRegs();
  regs.SP = 0x01F0;
  regs.A = 0x00FF;
  f.cpu.SetRegs(regs);
  f.wram.WriteRegister(0x01F4, 0xFF);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x00);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

TEST_CASE("CMP stack-relative 8-bit equal result", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xC3);
  f.SetRomByte(0x0001U, 0x04);
  f.SyncCartridge();

  f.cpu.Reset();
  auto regs = f.cpu.GetRegs();
  regs.SP = 0x01F0;
  regs.A = 0x0010;
  f.cpu.SetRegs(regs);
  f.wram.WriteRegister(0x01F4, 0x10);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  // A is unchanged by CMP.
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x10);
}

// ============================================================================
// ALU sr,S 16-bit — one TEST_CASE per ALU mnemonic (FLAG-05).
// Cycle formula 5-m at m=0 = 5 cycles.
// ============================================================================

TEST_CASE("ADC stack-relative 16-bit uses 5 cycles", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x63);
  f.SetRomByte(0x0001U, 0x04);
  f.SyncCartridge();

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0x1000);
  auto regs = f.cpu.GetRegs();
  regs.SP = 0x01F0;
  regs.P.C = false;
  f.cpu.SetRegs(regs);
  f.wram.WriteRegister(0x01F4, 0x34);
  f.wram.WriteRegister(0x01F5, 0x12);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0x2234);
}

TEST_CASE("SBC stack-relative 16-bit", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xE3);
  f.SetRomByte(0x0001U, 0x04);
  f.SyncCartridge();

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0x0002);
  auto regs = f.cpu.GetRegs();
  regs.SP = 0x01F0;
  regs.P.C = true;
  f.cpu.SetRegs(regs);
  f.wram.WriteRegister(0x01F4, 0x01);
  f.wram.WriteRegister(0x01F5, 0x00);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0x0001);
  REQUIRE(f.cpu.GetRegs().P.C == true);
}

TEST_CASE("AND stack-relative 16-bit", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x23);
  f.SetRomByte(0x0001U, 0x04);
  f.SyncCartridge();

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0xFFFF);
  auto regs = f.cpu.GetRegs();
  regs.SP = 0x01F0;
  f.cpu.SetRegs(regs);
  f.wram.WriteRegister(0x01F4, 0x00);
  f.wram.WriteRegister(0x01F5, 0xFF);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0xFF00);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("ORA stack-relative 16-bit", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x03);
  f.SetRomByte(0x0001U, 0x04);
  f.SyncCartridge();

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0x00FF);
  auto regs = f.cpu.GetRegs();
  regs.SP = 0x01F0;
  f.cpu.SetRegs(regs);
  f.wram.WriteRegister(0x01F4, 0xFF);
  f.wram.WriteRegister(0x01F5, 0x00);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0x00FF);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}

TEST_CASE("EOR stack-relative 16-bit sets Z", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x43);
  f.SetRomByte(0x0001U, 0x04);
  f.SyncCartridge();

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0xFFFF);
  auto regs = f.cpu.GetRegs();
  regs.SP = 0x01F0;
  f.cpu.SetRegs(regs);
  f.wram.WriteRegister(0x01F4, 0xFF);
  f.wram.WriteRegister(0x01F5, 0xFF);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().A == 0x0000);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

TEST_CASE("CMP stack-relative 16-bit equal", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xC3);
  f.SetRomByte(0x0001U, 0x04);
  f.SyncCartridge();

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0x1234);
  auto regs = f.cpu.GetRegs();
  regs.SP = 0x01F0;
  f.cpu.SetRegs(regs);
  f.wram.WriteRegister(0x01F4, 0x34);
  f.wram.WriteRegister(0x01F5, 0x12);

  TickResult r = f.cpu.Tick(46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  // A unchanged.
  REQUIRE(f.cpu.GetRegs().A == 0x1234);
}

// ============================================================================
// sr,S does NOT pay the DL-nonzero penalty (T-04-02). Even with DP=$0123
// (DL=$23 != 0) the cycle count remains 4 for the 8-bit form, not 5.
// ============================================================================

TEST_CASE("ADC stack-relative does not pay DL-nonzero penalty", "[unit][opcode][cpu][sr]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x63);
  f.SetRomByte(0x0001U, 0x04);
  f.SyncCartridge();

  f.cpu.Reset();
  auto regs = f.cpu.GetRegs();
  regs.SP = 0x01F0;
  regs.A = 0x0001;
  regs.DP = 0x0123;  // DL = 0x23 != 0 — would trigger +w for dp modes, but not for sr,S.
  regs.P.C = false;
  f.cpu.SetRegs(regs);
  f.wram.WriteRegister(0x01F4, 0x05);

  TickResult r = f.cpu.Tick(38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x06);
}
