#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/rom/cart_profile.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/memory/wram.h"

namespace pupsnes::test {

struct ResetFixture {
  SNES snes;
  WRAM& wram;
  CPU& cpu;
  std::array<uint8_t, Cartridge::kLoROMWindowSize> rom{};

  // WRAM and CPU are stable members of SNES (constructed once, lifetime ==
  // SNES lifetime), so caching references is safe. Cartridge is not cached:
  // LoadRomWithProfile destroys and rebuilds the cart instance, which
  // would dangle a captured reference. Tests that need the cart re-fetch
  // via `snes.GetCartridge()` on demand.
  ResetFixture() : wram(snes.GetWram()), cpu(snes.GetCpu()) {
    rom.fill(0xEA);
    SetResetVector(0x8000);
    SyncCartridge();
  }

  void SetResetVector(uint16_t address) {
    rom[0x7FFCU] = static_cast<uint8_t>(address & 0x00FFU);
    rom[0x7FFDU] = static_cast<uint8_t>(address >> 8U);
  }

  void SetRomByte(std::size_t offset, uint8_t value) { rom[offset] = value; }

  void SyncCartridge() {
    // Test ROMs are NOP-filled (0xEA) without a real header — auto-detect
    // would read 0xEA at $7FD6 as a Super Game Boy coprocessor and reject
    // the build (no SGB builder yet). Force the (kLoROM, kNone) builder.
    CartProfile profile{};
    profile.mapper = MapperKind::kLoROM;
    profile.coproc = Coprocessor::kNone;
    snes.LoadRomWithProfile(profile, rom);
  }

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
