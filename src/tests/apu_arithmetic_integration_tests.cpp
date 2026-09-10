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

constexpr TimeMasterT kRunBudget = 1'000'000;
constexpr std::size_t kPayloadRomOffset = 0x0100;
constexpr uint16_t kUploadAddress = 0x0200;
constexpr std::size_t kUploadSize = 0x0300;
constexpr std::size_t kResponseOffset = 0x02E0;

struct ArithmeticSnapshot {
  std::array<uint64_t, 12> cpu_state{};
  std::array<uint64_t, 12> spc_state{};
  std::array<uint8_t, pupsnes::Apu::kRamSize> aram{};
  std::array<uint8_t, 4> input_ports{};
  std::array<uint8_t, 4> output_ports{};
  std::array<uint8_t, 5> cpu_signature{};
  std::array<TimeMasterT, 6> clocks{};
};

std::vector<uint8_t> ReadArithmeticRom() {
  const auto path = std::filesystem::path(PUPSNES_TEST_ROM_DIR) / "apu_arithmetic.sfc";
  std::ifstream stream(path, std::ios::binary);
  INFO("ROM path: " << path);
  REQUIRE(stream.good());
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

ArithmeticSnapshot RunArithmetic(std::span<const uint8_t> rom, std::span<const TimeMasterT> slices) {
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
          FAIL("65C816 fault during SPC700 arithmetic ROM execution");
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
  REQUIRE(spc.pc == 0x04EF);
  REQUIRE(spc.x == 0x10);
  REQUIRE(spc.y == 0x20);
  REQUIRE(spc.sp == 0xEF);

  ArithmeticSnapshot snapshot;
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
  snapshot.clocks = {snes.GetMasterTime(), cpu.GetTime(),           apu.GetTime(),
                     apu.GetClockPhase(),  apu.GetDspSampleCount(), apu.GetDspClockPhase()};
  return snapshot;
}

void RequireArithmeticResults(const ArithmeticSnapshot& snapshot, std::span<const uint8_t> rom) {
  // Hand-calculated result triples: low/A, high/Y, PSW. These are literal
  // arithmetic expectations independent of the instruction implementation.
  constexpr std::array<std::array<uint8_t, 3>, 27> kExpected = {{
      {0x80, 0x00, 0xC8},  // $7F + 1: signed overflow and half carry.
      {0x00, 0x00, 0x43},  // $80 + $80: carry, signed overflow, zero.
      {0xFF, 0x00, 0x80},  // $00 - 1 with carry set: borrow.
      {0x00, 0x00, 0x0B},  // $FF + [$40=1] with carry clear.
      {0x00, 0x00, 0x0B},  // ADC [$41=$FF],[$40=1].
      {0xFF, 0x00, 0x80},  // SBC [$41=0],#1 with carry set.
      {0x00, 0x00, 0x02},  // INC [$41=$FF].
      {0xFF, 0x00, 0x80},  // DEC [$0041=0].
      {0x00, 0x80, 0xA0},  // INCW $7FFF -> $8000 with P=1.
      {0xFF, 0x7F, 0x20},  // DECW $8000 -> $7FFF with P=1.
      {0x00, 0x80, 0xC8},  // ADDW $7FFF + 1: bit-12 half carry.
      {0xFF, 0x7F, 0x41},  // SUBW $8000 - 1: signed overflow, no borrow.
      {0xFF, 0x7F, 0x41},  // CMPW $7FFF,1 preserves YA and overflow.
      {0x01, 0x00, 0x4B},  // CMPW 1,1 preserves V/H and sets Z/C.
      {0x5A, 0x00, 0xC8},  // CMP X=0,#1 preserves A and V/H.
      {0x5A, 0x3B, 0x49},  // CMP Y=$3B,[$0040=1] preserves A/Y and V/H.
      {0xFF, 0x3B, 0xC9},  // CMP [$41=$FF],[$40=1] preserves memory.
      {0x83, 0x00, 0x80},  // Decimal 45 + 38 = 83.
      {0x00, 0x00, 0x03},  // Decimal 99 + 1 = 00 with carry.
      {0x99, 0x00, 0x80},  // Decimal 00 - 1 = 99 with borrow.
      {0x09, 0x00, 0x01},  // Decimal 10 - 1 = 09 without borrow.
      {0x20, 0x01, 0x49},  // MUL $12 * $10 = $0120, preserving V/H/C.
      {0x60, 0x00, 0x01},  // DIV $0120 / 3 = $60, remainder 0.
      {0x23, 0x04, 0x49},  // DIV $1234 / $10: truncated quotient $23, r4.
      {0xEE, 0x20, 0xC9},  // DIV overflow path: $FF - $1000/$F0 = $EE.
      {0x18, 0x00, 0x08},  // Decimal 9 + 9 consumes H even with a low digit <= 9.
      {0xF0, 0x00, 0x4B},  // MUL $10 * $0F = $00F0 sets Z from the high byte only.
  }};
  for (std::size_t index = 0; index < kExpected.size(); ++index) {
    for (std::size_t field = 0; field < kExpected[index].size(); ++field) {
      INFO("Arithmetic result case: " << index << ", field (A/Y/PSW): " << field);
      REQUIRE(snapshot.aram[0x1000 + 3 * index + field] == kExpected[index][field]);
    }
  }

  constexpr std::array<std::pair<uint16_t, uint8_t>, 7> kMemoryResults = {{
      {0x0040, 0x01},
      {0x0041, 0xFF},
      {0x0050, 0x01},
      {0x0051, 0x00},
      {0x00F1, 0x00},
      {0x0100, 0x7F},
      {0x01FF, 0xFF},
  }};
  for (const auto& [address, value] : kMemoryResults) {
    INFO("Arithmetic result ARAM address: " << address);
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
  const auto spc_cycles = (kRunBudget * pupsnes::Apu::kClockNumerator) / pupsnes::Apu::kClockDenominator;
  REQUIRE(snapshot.clocks[4] == spc_cycles / 32);
  REQUIRE(snapshot.clocks[5] == spc_cycles % 32);
  const std::array<uint8_t, 4> expected_input = {0x01, 0x00, 0x00, 0x02};
  REQUIRE(snapshot.input_ports == expected_input);
}

void RequirePass(const ArithmeticSnapshot& snapshot) {
  const std::array<uint8_t, 5> signature = {'P', 'A', 'S', 'S', 0x62};
  const std::array<uint8_t, 4> output_ports = {0x01, 0xA5, 0x00, 0x62};
  REQUIRE(snapshot.cpu_signature == signature);
  REQUIRE(snapshot.output_ports == output_ports);
}

void RequireIdenticalArithmetic(const ArithmeticSnapshot& expected, const ArithmeticSnapshot& actual) {
  REQUIRE(actual.aram == expected.aram);
  REQUIRE(actual.cpu_state == expected.cpu_state);
  REQUIRE(actual.spc_state == expected.spc_state);
  REQUIRE(actual.input_ports == expected.input_ports);
  REQUIRE(actual.output_ports == expected.output_ports);
  REQUIRE(actual.cpu_signature == expected.cpu_signature);
  REQUIRE(actual.clocks == expected.clocks);
}

}  // namespace

TEST_CASE("A reset-booted ROM executes SPC700 arithmetic through real IPL upload",
          "[integration][rom][apu][spc700][arithmetic]") {
  const auto rom = ReadArithmeticRom();
  const std::array<TimeMasterT, 1> slices = {kRunBudget};
  const auto snapshot = RunArithmetic(rom, slices);
  RequireArithmeticResults(snapshot, rom);
  RequirePass(snapshot);
}

TEST_CASE("Changing an uploaded SPC700 addition to subtraction changes the CPU's result",
          "[integration][rom][apu][spc700][arithmetic]") {
  auto rom = ReadArithmeticRom();
  const auto instruction_offset = kPayloadRomOffset + kResponseOffset + 7;
  REQUIRE(rom.size() > instruction_offset + 1);
  REQUIRE(rom[instruction_offset] == 0x88);  // ADC A,#imm
  REQUIRE(rom[instruction_offset + 1] == 0x02);
  rom[instruction_offset] = 0xA8;  // SBC A,#2: $60 - 2 - borrow = $5D.

  const std::array<TimeMasterT, 1> slices = {kRunBudget};
  const auto snapshot = RunArithmetic(rom, slices);
  RequireArithmeticResults(snapshot, rom);
  const std::array<uint8_t, 5> failed_signature = {'F', 0, 0, 0, 0};
  const std::array<uint8_t, 4> output_ports = {0x01, 0xA5, 0x00, 0x5D};
  REQUIRE(snapshot.cpu_signature == failed_signature);
  REQUIRE(snapshot.output_ports == output_ports);
}

TEST_CASE("Uploaded SPC700 arithmetic is deterministic across master slice sizes",
          "[integration][rom][apu][spc700][arithmetic]") {
  const auto rom = ReadArithmeticRom();
  const std::array<TimeMasterT, 1> large_slices = {kRunBudget};
  const auto expected = RunArithmetic(rom, large_slices);
  RequireArithmeticResults(expected, rom);
  RequirePass(expected);

  SECTION("one master cycle per slice") {
    const std::array<TimeMasterT, 1> slices = {1};
    const auto actual = RunArithmetic(rom, slices);
    RequireArithmeticResults(actual, rom);
    RequirePass(actual);
    RequireIdenticalArithmetic(expected, actual);
  }
  SECTION("irregular slices across opcode and port access boundaries") {
    const std::array<TimeMasterT, 7> slices = {1, 19, 4079, 2, 127, 47, 5};
    const auto actual = RunArithmetic(rom, slices);
    RequireArithmeticResults(actual, rom);
    RequirePass(actual);
    RequireIdenticalArithmetic(expected, actual);
  }
}
