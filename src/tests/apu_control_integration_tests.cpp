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

constexpr TimeMasterT kRunBudget = 1'500'000;
constexpr std::size_t kPayloadRomOffset = 0x0100;
constexpr uint16_t kUploadAddress = 0x0200;
constexpr std::size_t kUploadSize = 0x0400;
constexpr std::size_t kResponseOffset = 0x03C0;

struct ControlSnapshot {
  std::array<uint64_t, 12> cpu_state{};
  std::array<uint64_t, 12> spc_state{};
  std::array<uint8_t, pupsnes::Apu::kRamSize> aram{};
  std::array<uint8_t, 4> input_ports{};
  std::array<uint8_t, 4> output_ports{};
  std::array<uint8_t, 5> cpu_signature{};
  std::array<TimeMasterT, 4> clocks{};
};

std::vector<uint8_t> ReadControlRom() {
  const auto path = std::filesystem::path(PUPSNES_TEST_ROM_DIR) / "apu_control.sfc";
  std::ifstream stream(path, std::ios::binary);
  INFO("ROM path: " << path);
  REQUIRE(stream.good());
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

ControlSnapshot RunControlProgram(std::span<const uint8_t> rom, std::span<const TimeMasterT> slices,
                                  bool stop = false) {
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
          FAIL("65C816 fault during SPC700 control-flow ROM execution");
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
  REQUIRE(spc.stopped == stop);
  REQUIRE(spc.sleeping == !stop);
  REQUIRE(spc.pc == 0x05EA);
  REQUIRE(spc.x == 0x00);
  REQUIRE(spc.y == 0x00);
  REQUIRE(spc.sp == 0xEF);

  ControlSnapshot snapshot;
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

void RequireControlResults(const ControlSnapshot& snapshot, std::span<const uint8_t> rom) {
  // Branch/call counts, logical results, restored stack pointer and BRK/RETI flags.
  // These expectations follow the program's operations, independently of its bytes.
  constexpr std::array<uint8_t, 16> kExpected = {
      8, 16, 3, 4, 1, 16, 1, 1, 0xA0, 0xDC, 1, 3, 1, 0xEF, 0x31, 0x25,
  };
  for (std::size_t index = 0; index < kExpected.size(); ++index) {
    INFO("Control result index: " << index);
    REQUIRE(snapshot.aram[0x1000 + index] == kExpected[index]);
  }
  REQUIRE(snapshot.aram[0x0066] == 0);
  REQUIRE(snapshot.aram[0x006A] == 0);
  REQUIRE(snapshot.aram[0x1100] == 0xDC);
  REQUIRE(snapshot.aram[0xFF00] == 0xAB);
  REQUIRE(snapshot.aram[0xFF01] == 0x63);
  REQUIRE(snapshot.aram[0xFF02] == 0x6F);

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

void RequirePass(const ControlSnapshot& snapshot) {
  const std::array<uint8_t, 5> signature = {'P', 'A', 'S', 'S', 0x92};
  const std::array<uint8_t, 4> output_ports = {0x01, 0xA5, 0x00, 0x92};
  REQUIRE(snapshot.cpu_signature == signature);
  REQUIRE(snapshot.output_ports == output_ports);
}

void RequireIdenticalControlProgram(const ControlSnapshot& expected, const ControlSnapshot& actual) {
  REQUIRE(actual.aram == expected.aram);
  REQUIRE(actual.cpu_state == expected.cpu_state);
  REQUIRE(actual.spc_state == expected.spc_state);
  REQUIRE(actual.input_ports == expected.input_ports);
  REQUIRE(actual.output_ports == expected.output_ports);
  REQUIRE(actual.cpu_signature == expected.cpu_signature);
  REQUIRE(actual.clocks == expected.clocks);
}

}  // namespace

TEST_CASE("A reset-booted ROM runs SPC700 branches nested calls all TCALL vectors and BRK RETI",
          "[integration][rom][apu][spc700][control]") {
  const auto rom = ReadControlRom();
  const std::array<TimeMasterT, 1> slices = {kRunBudget};
  const auto snapshot = RunControlProgram(rom, slices);
  RequireControlResults(snapshot, rom);
  RequirePass(snapshot);
}

TEST_CASE("Changing the uploaded SPC700 logic result changes the CPU's verdict",
          "[integration][rom][apu][spc700][control]") {
  auto rom = ReadControlRom();
  const auto opcode_offset = kPayloadRomOffset + kResponseOffset + 34;
  REQUIRE(rom.size() > opcode_offset + 1);
  REQUIRE(rom[opcode_offset] == 0x48);  // EOR A,#imm
  REQUIRE(rom[opcode_offset + 1] == 0xA5);
  rom[opcode_offset + 1] = 0xA4;  // Computed $37 XOR $A4 = $93.
  const std::array<TimeMasterT, 1> slices = {kRunBudget};
  const auto snapshot = RunControlProgram(rom, slices);
  RequireControlResults(snapshot, rom);
  const std::array<uint8_t, 5> failed_signature = {'F', 0, 0, 0, 0};
  const std::array<uint8_t, 4> output_ports = {0x01, 0xA5, 0x00, 0x93};
  REQUIRE(snapshot.cpu_signature == failed_signature);
  REQUIRE(snapshot.output_ports == output_ports);
}

TEST_CASE("Uploaded SPC700 control flow and halted execution are deterministic across master slices",
          "[integration][rom][apu][spc700][control]") {
  for (const bool stop : {false, true}) {
    CAPTURE(stop);
    auto rom = ReadControlRom();
    const auto halt_offset = kPayloadRomOffset + kResponseOffset + 41;
    REQUIRE(rom.size() > halt_offset);
    REQUIRE(rom[halt_offset] == 0xEF);
    if (stop) rom[halt_offset] = 0xFF;
    const std::array<TimeMasterT, 1> large_slices = {kRunBudget};
    const auto expected = RunControlProgram(rom, large_slices, stop);
    RequireControlResults(expected, rom);
    RequirePass(expected);
    const std::array<TimeMasterT, 1> single_cycles = {1};
    const std::array<TimeMasterT, 7> irregular = {1, 19, 4079, 2, 127, 47, 5};
    for (const std::span<const TimeMasterT> slices :
         {std::span<const TimeMasterT>(single_cycles), std::span<const TimeMasterT>(irregular)}) {
      const auto actual = RunControlProgram(rom, slices, stop);
      RequireControlResults(actual, rom);
      RequirePass(actual);
      RequireIdenticalControlProgram(expected, actual);
    }
  }
}
