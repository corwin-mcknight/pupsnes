#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

#include "pupsnes/core/scheduler.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/apu/apu.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/memory/wram.h"

namespace {

using pupsnes::CPU;
using pupsnes::SNES;
using pupsnes::TimeMasterT;

constexpr TimeMasterT kRunBudget = 500'000;
constexpr std::size_t kPayloadRomOffset = 0x0100;
constexpr uint16_t kUploadAddress = 0x0200;
constexpr std::size_t kUploadSize = 0x0110;
constexpr uint16_t kSecondUploadAddress = 0x0400;
constexpr std::size_t kSecondUploadSize = 4;

struct UploadSnapshot {
  CPU::Regs cpu;
  uint64_t cpu_instructions = 0;
  std::array<uint64_t, 12> spc_state{};
  std::array<uint8_t, 0x10000> aram{};
  std::array<uint8_t, 4> input_ports{};
  std::array<uint8_t, 4> output_ports{};
  std::array<uint8_t, 5> signature{};
  TimeMasterT master_time = 0;
  TimeMasterT cpu_time = 0;
  TimeMasterT apu_time = 0;
  TimeMasterT apu_clock_phase = 0;
};

std::vector<uint8_t> ReadUploadRom() {
  const auto path = std::filesystem::path(PUPSNES_TEST_ROM_DIR) / "apu_upload.sfc";
  std::ifstream stream(path, std::ios::binary);
  INFO("ROM path: " << path);
  REQUIRE(stream.good());
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

UploadSnapshot RunUpload(std::span<const uint8_t> rom, std::span<const TimeMasterT> slices) {
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
          FAIL("65C816 fault during IPL upload");
        }
      }
      snes.MachineSync(snes.GetMasterTime());
      scheduler.FireEventsThrough(snes.GetMasterTime());
    }
  }

  const auto& apu = snes.GetApu();
  const auto& spc = apu.GetCpu().GetState();
  INFO("65C816 PC: " << snes.GetCpu().GetRegs().PC << ", SPC700 PC: " << spc.pc
                     << ", SPC700 fault opcode: " << static_cast<unsigned>(spc.fault_opcode));
  REQUIRE_FALSE(snes.GetCpu().GetFault().has_value());
  REQUIRE_FALSE(apu.GetFault().has_value());
  REQUIRE_FALSE(spc.faulted);
  REQUIRE(snes.GetCpu().GetHaltState() == pupsnes::HaltState::kStp);
  REQUIRE(spc.stopped);
  REQUIRE_FALSE(spc.sleeping);
  REQUIRE(spc.pc == 0x0214);

  UploadSnapshot snapshot;
  snapshot.cpu = snes.GetCpu().GetRegs();
  snapshot.cpu_instructions = snes.GetCpu().GetRetiredInstructionCount();
  snapshot.spc_state = {spc.pc,     spc.a,       spc.x,        spc.y,       spc.sp,           spc.psw,
                        spc.cycles, spc.stopped, spc.sleeping, spc.faulted, spc.fault_opcode, spc.fault_pc};
  for (std::size_t address = 0; address < snapshot.aram.size(); ++address) {
    snapshot.aram[address] = apu.PeekRam(static_cast<uint16_t>(address));
  }
  for (std::size_t port = 0; port < snapshot.input_ports.size(); ++port) {
    snapshot.input_ports[port] = apu.GetInputPort(port);
    snapshot.output_ports[port] = apu.GetPort(port);
  }
  for (std::size_t address = 0; address < snapshot.signature.size(); ++address) {
    snapshot.signature[address] = snes.GetWram().Peek(static_cast<uint32_t>(address));
  }
  snapshot.master_time = snes.GetMasterTime();
  snapshot.cpu_time = snes.GetCpu().GetTime();
  snapshot.apu_time = apu.GetTime();
  snapshot.apu_clock_phase = apu.GetClockPhase();
  return snapshot;
}

void RequireCompletedUpload(const UploadSnapshot& snapshot, std::span<const uint8_t> rom) {
  const std::array<uint8_t, 5> expected_signature = {'P', 'A', 'S', 'S', 0x42};
  REQUIRE(snapshot.signature == expected_signature);
  REQUIRE(snapshot.master_time == kRunBudget);
  REQUIRE(snapshot.cpu_time == kRunBudget);
  REQUIRE(snapshot.apu_time == kRunBudget);
  REQUIRE(snapshot.aram[0x10] == 0x42);
  const std::array<uint8_t, 4> expected_input = {0x77, 0x00, 0x41, 0x02};
  const std::array<uint8_t, 4> expected_output = {0x05, 0xA5, 0x00, 0x42};
  REQUIRE(snapshot.input_ports == expected_input);
  REQUIRE(snapshot.output_ports == expected_output);

  REQUIRE(rom.size() >= kPayloadRomOffset + kUploadSize + kSecondUploadSize);
  for (std::size_t offset = 0; offset < kUploadSize; ++offset) {
    INFO("Uploaded ARAM address: " << kUploadAddress + offset);
    REQUIRE(snapshot.aram[kUploadAddress + offset] == rom[kPayloadRomOffset + offset]);
  }
  REQUIRE(snapshot.aram[0x02FF] == 0xA6);
  REQUIRE(snapshot.aram[0x0300] == 0xA6);
  REQUIRE(snapshot.aram[0x030F] == 0x3C);
  REQUIRE(snapshot.aram[0x0310] == 0x00);
  for (std::size_t offset = 0; offset < kSecondUploadSize; ++offset) {
    INFO("Second upload ARAM address: " << kSecondUploadAddress + offset);
    REQUIRE(snapshot.aram[kSecondUploadAddress + offset] == rom[kPayloadRomOffset + kUploadSize + offset]);
  }
}

void RequireIdenticalExecution(const UploadSnapshot& expected, const UploadSnapshot& actual) {
  REQUIRE(actual.aram == expected.aram);
  REQUIRE(actual.input_ports == expected.input_ports);
  REQUIRE(actual.output_ports == expected.output_ports);
  REQUIRE(actual.signature == expected.signature);
  REQUIRE(actual.spc_state == expected.spc_state);
  REQUIRE(actual.cpu_instructions == expected.cpu_instructions);
  REQUIRE(actual.cpu.A == expected.cpu.A);
  REQUIRE(actual.cpu.X == expected.cpu.X);
  REQUIRE(actual.cpu.Y == expected.cpu.Y);
  REQUIRE(actual.cpu.SP == expected.cpu.SP);
  REQUIRE(actual.cpu.DP == expected.cpu.DP);
  REQUIRE(actual.cpu.PBR == expected.cpu.PBR);
  REQUIRE(actual.cpu.DBR == expected.cpu.DBR);
  REQUIRE(actual.cpu.PC == expected.cpu.PC);
  REQUIRE(actual.cpu.P.ToByte() == expected.cpu.P.ToByte());
  REQUIRE(actual.cpu.P.E == expected.cpu.P.E);
  REQUIRE(actual.master_time == expected.master_time);
  REQUIRE(actual.cpu_time == expected.cpu_time);
  REQUIRE(actual.apu_time == expected.apu_time);
  REQUIRE(actual.apu_clock_phase == expected.apu_clock_phase);
}

}  // namespace

TEST_CASE("A reset-booted ROM uploads and executes an SPC700 responder through the IPL", "[integration][rom][apu]") {
  const auto rom = ReadUploadRom();
  const std::array<TimeMasterT, 1> slices = {kRunBudget};
  const auto snapshot = RunUpload(rom, slices);
  RequireCompletedUpload(snapshot, rom);
}

TEST_CASE("The CPU's upload verdict depends on the instructions in the SPC700 payload", "[integration][rom][apu]") {
  auto rom = ReadUploadRom();
  constexpr std::size_t kIncrementOffset = kPayloadRomOffset + 11;
  REQUIRE(rom.size() > kIncrementOffset);
  REQUIRE(rom[kIncrementOffset] == 0xBC);  // INC A
  rom[kIncrementOffset] = 0x00;            // NOP: return the command unchanged.

  const std::array<TimeMasterT, 1> slices = {kRunBudget};
  const auto snapshot = RunUpload(rom, slices);
  const std::array<uint8_t, 5> failed_signature = {'F', 0, 0, 0, 0};
  REQUIRE(snapshot.signature == failed_signature);
  REQUIRE(snapshot.aram[0x10] == 0x41);
  REQUIRE(snapshot.output_ports[3] == 0x41);
  REQUIRE(snapshot.input_ports[2] == 0x41);
}

TEST_CASE("SPC700 IPL upload and reply are identical across master execution slice sizes", "[integration][rom][apu]") {
  const auto rom = ReadUploadRom();
  const std::array<TimeMasterT, 1> large_slices = {kRunBudget};
  const auto expected = RunUpload(rom, large_slices);
  RequireCompletedUpload(expected, rom);

  SECTION("one master cycle per slice") {
    const std::array<TimeMasterT, 1> slices = {1};
    const auto actual = RunUpload(rom, slices);
    RequireCompletedUpload(actual, rom);
    RequireIdenticalExecution(expected, actual);
  }
  SECTION("irregular slices across CPU and APU instruction boundaries") {
    const std::array<TimeMasterT, 7> slices = {3, 29, 1, 4093, 17, 64, 251};
    const auto actual = RunUpload(rom, slices);
    RequireCompletedUpload(actual, rom);
    RequireIdenticalExecution(expected, actual);
  }
}
