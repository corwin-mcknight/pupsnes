#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

#include "pupsnes/core/scheduler.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/apu/apu.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/memory/wram.h"

namespace {

using pupsnes::SNES;
using pupsnes::TimeMasterT;

constexpr TimeMasterT kRunBudget = 750'000;
constexpr std::size_t kPayloadRomOffset = 0x0100;
constexpr uint16_t kUploadAddress = 0x0200;
constexpr std::size_t kUploadSize = 0x0200;
constexpr std::size_t kResponseOffset = 0x01E0;

struct TransferSnapshot {
  std::array<uint64_t, 12> cpu_state{};
  std::array<uint64_t, 12> spc_state{};
  std::array<uint8_t, pupsnes::Apu::kRamSize> aram{};
  std::array<uint8_t, 4> input_ports{};
  std::array<uint8_t, 4> output_ports{};
  std::array<uint8_t, 5> cpu_signature{};
  std::array<TimeMasterT, 4> clocks{};
};

std::vector<uint8_t> ReadTransferRom() {
  const auto path = std::filesystem::path(PUPSNES_TEST_ROM_DIR) / "apu_transfers.sfc";
  std::ifstream stream(path, std::ios::binary);
  INFO("ROM path: " << path);
  REQUIRE(stream.good());
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

TransferSnapshot RunTransfers(std::span<const uint8_t> rom, std::span<const TimeMasterT> slices) {
  REQUIRE_FALSE(slices.empty());
  REQUIRE(std::ranges::all_of(slices, [](TimeMasterT slice) { return slice > 0; }));
  SNES snes;
  const auto load = snes.LoadRom(rom);
  INFO(load.message);
  REQUIRE(load.ok);
  snes.Reset();

  std::size_t slice_index = 0;
  while (snes.GetMasterTime() < kRunBudget) {
    const auto slice_target = std::min(kRunBudget, snes.GetMasterTime() + slices[slice_index++ % slices.size()]);
    while (snes.GetMasterTime() < slice_target) {
      auto& scheduler = snes.GetScheduler();
      auto target = slice_target;
      if (scheduler.HasPendingEvents()) {
        target = std::min(target, scheduler.NextEventMasterTime());
      }
      if (snes.GetMasterTime() < target) {
        const auto result = snes.GetCpu().TickToTarget(target);
        if (result.reason == pupsnes::TickStopReason::kFault) {
          FAIL("65C816 fault during SPC700 transfer ROM execution");
        }
      }
      snes.MachineSync(snes.GetMasterTime());
      scheduler.FireEventsThrough(snes.GetMasterTime());
    }
  }

  const auto& apu = snes.GetApu();
  const auto& spc = apu.GetCpu().GetState();
  const auto& cpu = snes.GetCpu();
  const auto regs = cpu.GetRegs();
  INFO("65C816 PC: " << regs.PC << ", SPC700 PC: " << spc.pc
                     << ", SPC700 fault opcode: " << static_cast<unsigned>(spc.fault_opcode));
  REQUIRE_FALSE(cpu.GetFault().has_value());
  REQUIRE_FALSE(apu.GetFault().has_value());
  REQUIRE_FALSE(spc.faulted);
  REQUIRE(cpu.GetHaltState() == pupsnes::HaltState::kStp);
  REQUIRE(spc.stopped);
  REQUIRE_FALSE(spc.sleeping);
  REQUIRE(spc.pc == 0x03E9);
  REQUIRE(spc.x == 0x01);
  REQUIRE(spc.y == 0x4C);
  REQUIRE(spc.sp == 0x01);

  TransferSnapshot snapshot;
  snapshot.cpu_state = {regs.A,
                        regs.X,
                        regs.Y,
                        regs.SP,
                        regs.DP,
                        regs.PBR,
                        regs.DBR,
                        regs.PC,
                        regs.P.ToByte(),
                        regs.P.E,
                        cpu.GetMicroOpIndex(),
                        cpu.GetRetiredInstructionCount()};
  snapshot.spc_state = {spc.pc,     spc.a,       spc.x,        spc.y,       spc.sp,           spc.psw,
                        spc.cycles, spc.stopped, spc.sleeping, spc.faulted, spc.fault_opcode, spc.fault_pc};
  snapshot.aram = apu.GetRam();
  for (std::size_t port = 0; port < snapshot.input_ports.size(); ++port) {
    snapshot.input_ports[port] = apu.GetInputPort(port);
    snapshot.output_ports[port] = apu.GetPort(port);
  }
  for (std::size_t address = 0; address < snapshot.cpu_signature.size(); ++address) {
    snapshot.cpu_signature[address] = snes.GetWram().Peek(static_cast<uint32_t>(address));
  }
  snapshot.clocks = {snes.GetMasterTime(), cpu.GetTime(), apu.GetTime(), apu.GetClockPhase()};
  return snapshot;
}

void RequireTransferResults(const TransferSnapshot& snapshot, std::span<const uint8_t> rom) {
  // Independent expected results of the uploaded instructions, not bytes copied
  // from the ROM. Each entry corresponds to SPC_SAVE or an absolute X/Y store.
  constexpr std::array<uint8_t, 28> kExpected = {
      0x23, 0x8C, 0x23, 0x8C, 0x8C, 0x5A, 0x6B, 0xA2, 0xB4, 0xB4, 0xE7, 0xD6, 0xE7, 0x01,
      0x91, 0x82, 0x73, 0x64, 0x55, 0x57, 0x56, 0x20, 0x2A, 0x3B, 0x4C, 0x01, 0x23, 0x8C,
  };
  for (std::size_t index = 0; index < kExpected.size(); ++index) {
    INFO("Transfer signature index: " << index);
    REQUIRE(snapshot.aram[0x1000 + index] == kExpected[index]);
  }

  constexpr std::array<std::pair<uint16_t, uint8_t>, 15> kMemoryResults = {{
      {0x0000, 0x73},
      {0x0001, 0x64},
      {0x00F1, 0x00},
      {0x0100, 0x3B},
      {0x0101, 0x2A},
      {0x0102, 0x01},
      {0x0103, 0x5A},
      {0x015E, 0x6B},
      {0x01FE, 0x20},
      {0x01FF, 0x4C},
      {0x1100, 0x80},
      {0x1101, 0x56},
      {0x1200, 0x8C},
      {0xFFFE, 0x55},
      {0xFFFF, 0x91},
  }};
  for (const auto& [address, value] : kMemoryResults) {
    INFO("Transfer result ARAM address: " << address);
    REQUIRE(snapshot.aram[address] == value);
  }

  REQUIRE(rom.size() >= kPayloadRomOffset + kUploadSize);
  for (std::size_t offset = 0; offset < kUploadSize; ++offset) {
    INFO("Uploaded program ARAM address: " << kUploadAddress + offset);
    REQUIRE(snapshot.aram[kUploadAddress + offset] == rom[kPayloadRomOffset + offset]);
  }
  REQUIRE(snapshot.clocks[0] == kRunBudget);
  REQUIRE(snapshot.clocks[1] == kRunBudget);
  REQUIRE(snapshot.clocks[2] == kRunBudget);
  REQUIRE(snapshot.clocks[3] == (kRunBudget * pupsnes::Apu::kClockNumerator) % pupsnes::Apu::kClockDenominator);
  const std::array<uint8_t, 4> expected_input = {0x01, 0x00, 0x00, 0x02};
  REQUIRE(snapshot.input_ports == expected_input);
}

void RequirePass(const TransferSnapshot& snapshot) {
  const std::array<uint8_t, 5> signature = {'P', 'A', 'S', 'S', 0x23};
  const std::array<uint8_t, 4> output_ports = {0x01, 0xA5, 0x00, 0x23};
  REQUIRE(snapshot.cpu_signature == signature);
  REQUIRE(snapshot.output_ports == output_ports);
}

void RequireIdenticalTransfers(const TransferSnapshot& expected, const TransferSnapshot& actual) {
  REQUIRE(actual.aram == expected.aram);
  REQUIRE(actual.cpu_state == expected.cpu_state);
  REQUIRE(actual.spc_state == expected.spc_state);
  REQUIRE(actual.input_ports == expected.input_ports);
  REQUIRE(actual.output_ports == expected.output_ports);
  REQUIRE(actual.cpu_signature == expected.cpu_signature);
  REQUIRE(actual.clocks == expected.clocks);
}

}  // namespace

TEST_CASE("A reset-booted ROM executes SPC700 data transfers through real IPL upload",
          "[integration][rom][apu][spc700][mov]") {
  const auto rom = ReadTransferRom();
  const std::array<TimeMasterT, 1> slices = {kRunBudget};
  const auto snapshot = RunTransfers(rom, slices);
  RequireTransferResults(snapshot, rom);
  RequirePass(snapshot);
}

TEST_CASE("Changing an uploaded SPC700 MOV source changes the CPU's result", "[integration][rom][apu][spc700][mov]") {
  auto rom = ReadTransferRom();
  const auto instruction_offset = kPayloadRomOffset + kResponseOffset;
  REQUIRE(rom.size() > instruction_offset + 2);
  REQUIRE(rom[instruction_offset] == 0xE5);
  REQUIRE(rom[instruction_offset + 1] == 0x00);
  REQUIRE(rom[instruction_offset + 2] == 0x10);
  rom[instruction_offset + 1] = 0x01;  // MOV A,!$1001 returns $8C instead of $23.

  const std::array<TimeMasterT, 1> slices = {kRunBudget};
  const auto snapshot = RunTransfers(rom, slices);
  RequireTransferResults(snapshot, rom);
  const std::array<uint8_t, 5> failed_signature = {'F', 0, 0, 0, 0};
  const std::array<uint8_t, 4> output_ports = {0x01, 0xA5, 0x00, 0x8C};
  REQUIRE(snapshot.cpu_signature == failed_signature);
  REQUIRE(snapshot.output_ports == output_ports);
}

TEST_CASE("Uploaded SPC700 transfers are deterministic across master slice sizes",
          "[integration][rom][apu][spc700][mov]") {
  const auto rom = ReadTransferRom();
  const std::array<TimeMasterT, 1> large_slices = {kRunBudget};
  const auto expected = RunTransfers(rom, large_slices);
  RequireTransferResults(expected, rom);
  RequirePass(expected);

  SECTION("one master cycle per slice") {
    const std::array<TimeMasterT, 1> slices = {1};
    const auto actual = RunTransfers(rom, slices);
    RequireTransferResults(actual, rom);
    RequirePass(actual);
    RequireIdenticalTransfers(expected, actual);
  }
  SECTION("irregular slices across opcode and port access boundaries") {
    const std::array<TimeMasterT, 7> slices = {1, 19, 4079, 2, 127, 47, 5};
    const auto actual = RunTransfers(rom, slices);
    RequireTransferResults(actual, rom);
    RequirePass(actual);
    RequireIdenticalTransfers(expected, actual);
  }
}
