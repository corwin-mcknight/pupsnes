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

using pupsnes::SdspMode;
using pupsnes::SNES;
using pupsnes::TimeMasterT;

constexpr TimeMasterT kRunBudget = 1'000'000;
constexpr std::size_t kPayloadRomOffset = 0x0100;
constexpr uint16_t kUploadAddress = 0x0200;
constexpr std::size_t kUploadSize = 0x0200;
constexpr std::size_t kEnableOffset = 0x0100;

struct HardwareSnapshot {
  std::array<uint64_t, 12> cpu_state{};
  std::array<uint64_t, 12> spc_state{};
  std::array<uint8_t, pupsnes::Apu::kRamSize> aram{};
  std::array<uint8_t, pupsnes::Sdsp::kRegisterCount> dsp_registers{};
  std::array<pupsnes::Apu::TimerState, 3> timers{};
  std::array<uint8_t, 4> input_ports{};
  std::array<uint8_t, 4> output_ports{};
  std::array<uint8_t, 5> cpu_signature{};
  std::array<TimeMasterT, 6> clocks{};
};

std::vector<uint8_t> ReadHardwareRom() {
  const auto path = std::filesystem::path(PUPSNES_TEST_ROM_DIR) / "apu_hardware.sfc";
  std::ifstream stream(path, std::ios::binary);
  INFO("ROM path: " << path);
  REQUIRE(stream.good());
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

HardwareSnapshot RunHardware(std::span<const uint8_t> rom, std::span<const TimeMasterT> slices,
                             SdspMode mode = SdspMode::kSimple) {
  REQUIRE_FALSE(slices.empty());
  REQUIRE(std::ranges::all_of(slices, [](TimeMasterT slice) { return slice > 0; }));
  SNES snes;
  snes.SetSdspModePending(mode);
  const auto load = snes.LoadRom(rom);
  INFO(load.message);
  REQUIRE(load.ok);
  snes.Reset();
  REQUIRE(snes.GetApu().GetDsp().Mode() == mode);

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
          FAIL("65C816 fault during uploaded SPC700 hardware test");
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

  HardwareSnapshot snapshot;
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
  for (std::size_t index = 0; index < snapshot.dsp_registers.size(); ++index) {
    snapshot.dsp_registers[index] = apu.GetDsp().ReadRegister(static_cast<uint8_t>(index));
  }
  for (std::size_t timer = 0; timer < snapshot.timers.size(); ++timer) {
    snapshot.timers[timer] = apu.GetTimerState(timer);
  }
  for (std::size_t port = 0; port < snapshot.input_ports.size(); ++port) {
    snapshot.input_ports[port] = apu.GetInputPort(port);
    snapshot.output_ports[port] = apu.GetPort(port);
  }
  for (std::size_t address = 0; address < snapshot.cpu_signature.size(); ++address) {
    snapshot.cpu_signature[address] = snes.GetWram().Peek(static_cast<uint32_t>(address));
  }
  snapshot.clocks = {snes.GetMasterTime(), cpu.GetTime(),           apu.GetTime(),
                     apu.GetClockPhase(),  apu.GetDspSampleCount(), apu.GetDspClockPhase()};
  return snapshot;
}

void RequireHardwareState(const HardwareSnapshot& snapshot, std::span<const uint8_t> rom) {
  // Ordinary DSP writes retain their complete byte. The high-bit write to
  // $8C is ignored, and writing $FF to ENDX leaves $7C cleared.
  std::array<uint8_t, pupsnes::Sdsp::kRegisterCount> expected_dsp{};
  expected_dsp[0x00] = 0x5A;
  expected_dsp[0x0C] = 0x35;
  expected_dsp[0x6C] = 0x60;
  REQUIRE(snapshot.dsp_registers == expected_dsp);

  constexpr std::array<std::pair<uint16_t, uint8_t>, 6> kMemoryResults = {{
      {0x00F1, 0x00},
      {0x00F2, 0x6C},
      {0x00F3, 0xFF},
      {0x00FA, 0x02},
      {0x00FB, 0x03},
      {0x00FC, 0x10},
  }};
  for (const auto& [address, value] : kMemoryResults) {
    INFO("Hardware register backing ARAM address: " << address);
    REQUIRE(snapshot.aram[address] == value);
  }
  REQUIRE(rom.size() >= kPayloadRomOffset + kUploadSize);
  for (std::size_t offset = 0; offset < kUploadSize; ++offset) {
    INFO("Uploaded program ARAM address: " << kUploadAddress + offset);
    REQUIRE(snapshot.aram[kUploadAddress + offset] == rom[kPayloadRomOffset + offset]);
  }

  const auto spc_cycles = (kRunBudget * pupsnes::Apu::kClockNumerator) / pupsnes::Apu::kClockDenominator;
  REQUIRE(snapshot.spc_state[6] == spc_cycles);
  REQUIRE(snapshot.clocks[0] == kRunBudget);
  REQUIRE(snapshot.clocks[1] == kRunBudget);
  REQUIRE(snapshot.clocks[2] == kRunBudget);
  REQUIRE(snapshot.clocks[3] == (kRunBudget * pupsnes::Apu::kClockNumerator) % pupsnes::Apu::kClockDenominator);
  REQUIRE(snapshot.clocks[4] == spc_cycles / 32);
  REQUIRE(snapshot.clocks[5] == spc_cycles % 32);
  constexpr std::array<uint8_t, 3> kTargets = {2, 3, 16};
  constexpr std::array<uint8_t, 3> kDivisors = {128, 128, 16};
  for (std::size_t index = 0; index < snapshot.timers.size(); ++index) {
    INFO("Timer index: " << index);
    const auto& timer = snapshot.timers[index];
    REQUIRE_FALSE(timer.enabled);
    REQUIRE(timer.target == kTargets[index]);
    REQUIRE(timer.divider == spc_cycles % kDivisors[index]);
  }
  const std::array<uint8_t, 4> expected_input = {0x01, 0x00, 0x00, 0x02};
  REQUIRE(snapshot.input_ports == expected_input);
}

void RequireHardwarePass(const HardwareSnapshot& snapshot) {
  // Literal expectations from DSP register semantics and one output tick
  // from each timer, with a second read returning zero after the first clears.
  constexpr std::array<uint8_t, 18> kExpected = {
      0x35, 0x35, 0x35, 0x8C, 0x5A, 0x00, 0x60, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA5,
  };
  for (std::size_t index = 0; index < kExpected.size(); ++index) {
    INFO("Hardware signature byte: " << index);
    REQUIRE(snapshot.aram[0x1000 + index] == kExpected[index]);
  }
  REQUIRE(snapshot.aram[0x00FD] == 0x0F);
  REQUIRE(snapshot.spc_state[0] == 0x03F3);
  REQUIRE(snapshot.spc_state[1] == 0x38);
  for (const auto& timer : snapshot.timers) {
    REQUIRE(timer.output == 0);
    REQUIRE(timer.counter < timer.target);
  }
  const std::array<uint8_t, 5> signature = {'P', 'A', 'S', 'S', 0x38};
  const std::array<uint8_t, 4> output_ports = {0x01, 0xA5, 0x00, 0x38};
  REQUIRE(snapshot.cpu_signature == signature);
  REQUIRE(snapshot.output_ports == output_ports);
}

void RequireIdenticalHardware(const HardwareSnapshot& expected, const HardwareSnapshot& actual) {
  REQUIRE(actual.aram == expected.aram);
  REQUIRE(actual.cpu_state == expected.cpu_state);
  REQUIRE(actual.spc_state == expected.spc_state);
  REQUIRE(actual.dsp_registers == expected.dsp_registers);
  REQUIRE(actual.timers == expected.timers);
  REQUIRE(actual.input_ports == expected.input_ports);
  REQUIRE(actual.output_ports == expected.output_ports);
  REQUIRE(actual.cpu_signature == expected.cpu_signature);
  REQUIRE(actual.clocks == expected.clocks);
}

}  // namespace

TEST_CASE("A reset-booted ROM accesses DSP registers and all three SPC timers through real IPL upload",
          "[integration][rom][apu][dsp][timer]") {
  const auto rom = ReadHardwareRom();
  const std::array<TimeMasterT, 1> slices = {kRunBudget};
  const auto snapshot = RunHardware(rom, slices);
  RequireHardwareState(snapshot, rom);
  RequireHardwarePass(snapshot);
}

TEST_CASE("An uploaded program that leaves timer zero disabled reaches the bounded CPU FAIL path",
          "[integration][rom][apu][dsp][timer]") {
  auto rom = ReadHardwareRom();
  const auto instruction_offset = kPayloadRomOffset + kEnableOffset;
  REQUIRE(rom.size() > instruction_offset + 2);
  REQUIRE(rom[instruction_offset] == 0x8F);  // MOV dp,#imm
  REQUIRE(rom[instruction_offset + 1] == 0x07);
  REQUIRE(rom[instruction_offset + 2] == 0xF1);
  rom[instruction_offset + 1] = 0x06;

  const std::array<TimeMasterT, 1> slices = {kRunBudget};
  const auto snapshot = RunHardware(rom, slices);
  RequireHardwareState(snapshot, rom);
  REQUIRE(snapshot.spc_state[0] == 0x0385);
  REQUIRE(snapshot.aram[0x1011] == 0xE0);
  REQUIRE(snapshot.aram[0x1007] == 0x00);
  REQUIRE(snapshot.timers[0].counter == 0);
  REQUIRE(snapshot.timers[0].output == 0);
  const std::array<uint8_t, 5> signature = {'F', 0, 0, 0, 0};
  const std::array<uint8_t, 4> output_ports = {0x01, 0xA5, 0x00, 0x00};
  REQUIRE(snapshot.cpu_signature == signature);
  REQUIRE(snapshot.output_ports == output_ports);
}

TEST_CASE("Uploaded DSP and timer operations preserve complete hardware state across master slices and backends",
          "[integration][rom][apu][dsp][timer]") {
  const auto rom = ReadHardwareRom();
  const std::array<TimeMasterT, 1> large_slices = {kRunBudget};
  const auto expected = RunHardware(rom, large_slices);
  RequireHardwareState(expected, rom);
  RequireHardwarePass(expected);

  SECTION("one master cycle per slice") {
    const std::array<TimeMasterT, 1> slices = {1};
    const auto actual = RunHardware(rom, slices);
    RequireHardwareState(actual, rom);
    RequireHardwarePass(actual);
    RequireIdenticalHardware(expected, actual);
  }
  SECTION("irregular slices across timer, DSP, and port edges") {
    const std::array<TimeMasterT, 9> slices = {1, 19, 4079, 2, 127, 47, 5, 673, 31};
    const auto actual = RunHardware(rom, slices);
    RequireHardwareState(actual, rom);
    RequireHardwarePass(actual);
    RequireIdenticalHardware(expected, actual);
  }
  SECTION("accurate backend receives the same hardware register trace") {
    const auto actual = RunHardware(rom, large_slices, SdspMode::kAccurate);
    RequireHardwareState(actual, rom);
    RequireHardwarePass(actual);
    RequireIdenticalHardware(expected, actual);
  }
}
