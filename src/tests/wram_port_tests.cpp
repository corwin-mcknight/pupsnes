#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "pupsnes/core/scheduler.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/5a22/dma_controller.h"
#include "pupsnes/memory/systembus.h"
#include "pupsnes/memory/wram.h"
#include "pupsnes/tools/trace_runner.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)

namespace {

struct WramPortFixture {
  SNES snes;
  TimeMasterT now = 0;

  void Write(SnesAddrT address, uint8_t data) {
    const auto plan = snes.system_bus->Plan(address, BusAccessType::kWrite, data);
    REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
    (void)snes.system_bus->Follow(plan, now++, 0);
  }

  uint8_t Read(SnesAddrT address) {
    const auto plan = snes.system_bus->Plan(address, BusAccessType::kRead);
    REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
    return snes.system_bus->Follow(plan, now++, 0).data;
  }

  void Address(uint32_t address) {
    Write(0x2181, static_cast<uint8_t>(address));
    Write(0x2182, static_cast<uint8_t>(address >> 8U));
    Write(0x2183, static_cast<uint8_t>(address >> 16U));
  }
};

}  // namespace

TEST_CASE("WRAM port writes are immediately visible through direct memory and MMIO mirrors", "[unit][wram-port]") {
  WramPortFixture f;
  f.Address(0x15000);
  // Banks $00-$3F and $80-$BF share one pointer and data port.
  for (uint32_t base : {0x000000U, 0x800000U}) {
    for (uint32_t bank = 0; bank < 0x40U; ++bank) {
      const uint8_t data = static_cast<uint8_t>(bank + (base == 0 ? 1U : 65U));
      const uint32_t offset = 0x15000U + data - 1U;
      f.Write(base | (bank << 16U) | 0x2180U, data);
      REQUIRE(f.snes.GetWram().Peek(offset) == data);
      REQUIRE(f.Read(0x7E0000U + offset) == data);
    }
  }
  // A direct access to this numerical offset is ordinary RAM, not MMIO.
  f.Write(0x7E2180, 0xA5);
  REQUIRE(f.Read(0x7E2180) == 0xA5);
  f.Write(0x2180, 0x5A);
  REQUIRE(f.snes.GetWram().Peek(0x15080) == 0x5A);
}

TEST_CASE("WRAM port reads and writes share a pointer and carry across page bank and memory boundaries",
          "[unit][wram-port]") {
  for (uint32_t address : {0x000FFU, 0x0FFFFU, 0x1FFFFU}) {
    CAPTURE(address);
    WramPortFixture f;
    f.Write(0x7E0000U + address, 0x12);
    f.Write(0x7E0000U + ((address + 2U) & 0x1FFFFU), 0x56);
    f.Address(address);
    REQUIRE(f.Read(0x2180) == 0x12);
    f.Write(0x2180, 0x34);
    REQUIRE(f.snes.GetWram().Peek((address + 1U) & 0x1FFFFU) == 0x34);
    REQUIRE(f.Read(0x802180) == 0x56);
    f.Address(address);
    f.Write(0x2180, 0xAB);
    f.Write(0x2180, 0xCD);
    REQUIRE(f.snes.GetWram().Peek(address) == 0xAB);
    REQUIRE(f.snes.GetWram().Peek((address + 1U) & 0x1FFFFU) == 0xCD);
  }
}

TEST_CASE("WRAM address bytes preserve other bits and ignore unused high bits", "[unit][wram-port]") {
  WramPortFixture f;
  f.Address(0x12345);
  f.Write(0xBF2181, 0x67);
  f.Write(0x2180, 0x12);
  REQUIRE(f.snes.GetWram().Peek(0x12367) == 0x12);
  f.Write(0x3F2182, 0xAB);
  f.Write(0x2180, 0x34);
  REQUIRE(f.snes.GetWram().Peek(0x1AB68) == 0x34);
  f.Write(0x2183, 0xFE);
  f.Write(0x2180, 0x56);
  REQUIRE(f.snes.GetWram().Peek(0x0AB69) == 0x56);
  f.Write(0x2183, 0xFF);
  f.Write(0x2180, 0x78);
  REQUIRE(f.snes.GetWram().Peek(0x1AB6A) == 0x78);
}

TEST_CASE("WRAM address registers and unused B-bus ports read open bus without advancing", "[unit][wram-port]") {
  WramPortFixture f;
  f.Address(0x12345);
  for (uint32_t address = 0x2181; address <= 0x21FF; ++address) {
    f.Write(0x7E0000, 0xA5);
    REQUIRE(f.Read(address) == 0xA5);
    if (address >= 0x2184) f.Write(address, 0xFF);
  }
  f.Write(0x2180, 0x12);
  REQUIRE(f.snes.GetWram().Peek(0x12345) == 0x12);
}

TEST_CASE("Machine reset resets the WRAM port pointer while retaining RAM", "[unit][wram-port]") {
  WramPortFixture f;
  f.Write(0x7E0000, 0x12);
  f.Address(0x12345);
  f.Write(0x2180, 0x34);
  f.snes.Reset();
  f.now = f.snes.GetMasterTime();
  REQUIRE(f.Read(0x2180) == 0x12);
  REQUIRE(f.snes.GetWram().Peek(0x12345) == 0x34);
  f.Write(0x2180, 0x56);
  REQUIRE(f.snes.GetWram().Peek(1) == 0x56);
}

TEST_CASE("CPU can execute code uploaded through WMDATA as in the FFIII boot sequence", "[unit][wram-port]") {
  // A small original program writes a routine through the ports, then jumps
  // to it. Missing $2180 writes leave a BRK at the destination, as FFIII did.
  constexpr std::array<uint8_t, 7> routine = {0xA9, 0x42, 0x8F, 0x00, 0x60, 0x7E, 0xDB};
  std::vector<uint8_t> program = {0xA9, 0x00, 0x8D, 0x81, 0x21,   // WMADDL = 0
                                  0xA9, 0x50, 0x8D, 0x82, 0x21,   // WMADDM = $50
                                  0xA9, 0x00, 0x8D, 0x83, 0x21};  // WMADDH = 0
  for (uint8_t byte : routine) {
    program.insert(program.end(), {0xA9, byte, 0x8D, 0x80, 0x21});
  }
  program.insert(program.end(), {0x5C, 0x00, 0x50, 0x7E});
  SNES snes;
  snes.Reset();
  for (uint32_t i = 0; i < program.size(); ++i) snes.GetWram().WriteRegister(0x1000U + i, program[i], 0);
  auto regs = snes.GetCpu().GetRegs();
  regs.PBR = 0x7E;
  regs.PC = 0x1000;
  snes.GetCpu().SetRegs(regs);
  REQUIRE_FALSE(tools::DriveMachineToMasterTime(snes, 4000).has_value());
  REQUIRE(snes.GetWram().Peek(0x6000) == 0x42);
  REQUIRE(snes.GetCpu().GetHaltState() == HaltState::kStp);
  REQUIRE(snes.GetCpu().GetRegs().PBR == 0x7E);
}

TEST_CASE("DMA from ROM to WMDATA fills WRAM and advances its port address", "[unit][wram-port][dma]") {
  WramPortFixture f;
  std::vector<uint8_t> rom(0x8000, 0xEA);
  rom[0x7FD5] = 0x20;
  rom[0x7FD6] = 0;
  rom[0x7FD8] = 0;
  rom[0] = 0x12;
  rom[1] = 0x34;
  rom[2] = 0x56;
  REQUIRE(f.snes.LoadRom(rom).ok);
  f.Address(0x1FFFF);
  f.Write(0x4300, 0);
  f.Write(0x4301, 0x80);
  f.Write(0x4302, 0);
  f.Write(0x4303, 0x80);
  f.Write(0x4304, 0);
  f.Write(0x4305, 3);
  f.Write(0x4306, 0);
  f.now = f.snes.GetDma().Trigger(1, f.now);
  REQUIRE(f.snes.GetWram().Peek(0x1FFFF) == 0x12);
  REQUIRE(f.snes.GetWram().Peek(0) == 0x34);
  REQUIRE(f.snes.GetWram().Peek(1) == 0x56);
  f.Write(0x2180, 0x78);
  REQUIRE(f.snes.GetWram().Peek(2) == 0x78);
}

TEST_CASE("DMA between WRAM and its own ports does not access or advance the port", "[unit][wram-port][dma]") {
  for (uint32_t a_addr : {0x7E1000U, 0x7F1000U, 0x001000U, 0x3F1000U, 0x801000U, 0xBF1000U}) {
    for (uint8_t port : std::array<uint8_t, 4>{0x80, 0x81, 0x82, 0x83}) {
      for (bool reverse : {false, true}) {
        CAPTURE(a_addr, port, reverse);
        WramPortFixture f;
        f.Write(a_addr, 0xA5);
        f.Write(0x7E5000, 0x12);
        f.Address(0x5000);
        f.Write(0x4300, reverse ? 0x80 : 0);
        f.Write(0x4301, port);
        f.Write(0x4302, static_cast<uint8_t>(a_addr));
        f.Write(0x4303, static_cast<uint8_t>(a_addr >> 8U));
        f.Write(0x4304, static_cast<uint8_t>(a_addr >> 16U));
        f.Write(0x4305, 1);
        f.Write(0x4306, 0);
        f.Write(0x7E6000, 0xCD);  // open bus for a blocked reverse transfer
        const auto start = f.now;
        f.now = f.snes.GetDma().Trigger(1, start);
        REQUIRE(f.now == start + 16U);
        REQUIRE(f.snes.GetDma().GetChannelState(0).das == 0);
        REQUIRE(f.Read(a_addr) == (reverse ? 0xCD : 0xA5));
        REQUIRE(f.Read(0x2180) == 0x12);
        f.Write(0x2180, 0x34);
        REQUIRE(f.snes.GetWram().Peek(0x5001) == 0x34);
      }
    }
  }
}

TEST_CASE("Reverse DMA from WMDATA to cartridge SRAM transfers bytes", "[unit][wram-port][dma]") {
  WramPortFixture f;
  std::vector<uint8_t> rom(0x8000, 0xEA);
  rom[0x7FD5] = 0x20;
  rom[0x7FD6] = 2;
  rom[0x7FD8] = 1;
  REQUIRE(f.snes.LoadRom(rom).ok);
  f.Write(0x7E5000, 0x12);
  f.Write(0x7E5001, 0x34);
  f.Write(0x7E5002, 0x56);
  f.Address(0x5000);
  f.Write(0x4300, 0x80);
  f.Write(0x4301, 0x80);
  f.Write(0x4302, 0);
  f.Write(0x4303, 0);
  f.Write(0x4304, 0x70);
  f.Write(0x4305, 2);
  f.Write(0x4306, 0);
  f.now = f.snes.GetDma().Trigger(1, f.now);
  REQUIRE(f.Read(0x700000) == 0x12);
  REQUIRE(f.Read(0x700001) == 0x34);
  REQUIRE(f.Read(0x2180) == 0x56);
}

TEST_CASE("HDMA from WRAM cannot write its own data or address ports", "[unit][wram-port][dma][hdma]") {
  WramPortFixture f;
  f.snes.Reset();
  f.now = f.snes.GetMasterTime();
  // One four-byte transfer targeting all four WRAM ports.
  constexpr std::array<uint8_t, 6> table = {1, 0xAA, 0xBB, 0xCC, 1, 0};
  for (uint32_t i = 0; i < table.size(); ++i) f.Write(0x7E1000U + i, table[i]);
  f.Write(0x7E5000, 0x12);
  f.Address(0x5000);
  f.Write(0x4300, 4);
  f.Write(0x4301, 0x80);
  f.Write(0x4302, 0);
  f.Write(0x4303, 0x10);
  f.Write(0x4304, 0x7E);
  f.Write(0x420C, 1);
  f.snes.MachineSync(30);
  f.snes.GetScheduler().FireEventsThrough(30);
  f.snes.MachineSync(1200);
  f.snes.GetScheduler().FireEventsThrough(1200);
  f.now = 1201;
  REQUIRE(f.snes.GetDma().GetChannelState(0).a2a == 5);
  REQUIRE(f.Read(0x2180) == 0x12);
  f.Write(0x2180, 0x34);
  REQUIRE(f.snes.GetWram().Peek(0x5001) == 0x34);
}
