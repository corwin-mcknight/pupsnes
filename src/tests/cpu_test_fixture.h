#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/memory/wram.h"

namespace pupsnes::test {

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

  // Writes the instruction bytes starting at the reset-vector entry point
  // (offset 0) and re-syncs the cartridge. Collapses the common
  // `SetRomByte(0, op); SetRomByte(1, lo); ... SyncCartridge();` pattern.
  void LoadInstruction(std::initializer_list<uint8_t> bytes) {
    std::size_t offset = 0;
    for (uint8_t b : bytes) {
      rom[offset++] = b;
    }
    SyncCartridge();
  }

  // Read-modify-write of CPU registers via a caller-supplied mutator.
  // Collapses the `auto r = cpu.GetRegs(); ...mutate...; cpu.SetRegs(r);` dance.
  template <typename Fn>
  void ModifyRegs(Fn&& fn) {
    auto regs = cpu.GetRegs();
    fn(regs);
    cpu.SetRegs(regs);
  }
};

inline void SetAccumulator16(CPU& cpu, uint16_t value) {
  auto regs = cpu.GetRegs();
  regs.P.E = false;
  regs.P.M = false;
  regs.A = value;
  cpu.SetRegs(regs);
}

inline void SetIndex16X(CPU& cpu, uint16_t value) {
  auto regs = cpu.GetRegs();
  regs.P.E = false;
  regs.P.X = false;
  regs.X = value;
  cpu.SetRegs(regs);
}

inline void SetIndex16Y(CPU& cpu, uint16_t value) {
  auto regs = cpu.GetRegs();
  regs.P.E = false;
  regs.P.X = false;
  regs.Y = value;
  cpu.SetRegs(regs);
}

inline void SetDataBank(CPU& cpu, uint8_t dbr) {
  auto regs = cpu.GetRegs();
  regs.DBR = dbr;
  cpu.SetRegs(regs);
}

}  // namespace pupsnes::test
