#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "pupsnes/core/snes.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/apu/apu.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/tools/trace_runner.h"

namespace {

std::filesystem::path TestRomPath(const std::string& name) {
  return std::filesystem::path(PUPSNES_TEST_ROM_DIR) / (name + ".sfc");
}

std::filesystem::path UniqueTempPath(const std::string& stem) {
  // Unique per test case so parallel Catch2 runs don't clobber each other.
  static std::atomic<uint64_t> counter{0};
  const auto n = counter.fetch_add(1, std::memory_order_relaxed);
  auto p = std::filesystem::temp_directory_path() / (stem + "." + std::to_string(n) + ".log");
  std::filesystem::remove(p);
  return p;
}

std::vector<std::string> ReadAllLines(const std::filesystem::path& p) {
  std::ifstream in(p);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(std::move(line));
  }
  return lines;
}

}  // namespace

TEST_CASE("RunTrace emits a versioned header and one line per retired instruction", "[unit][trace_runner]") {
  pupsnes::tools::TraceRunOptions opts;
  opts.rom_path = TestRomPath("instruction_smoke");
  opts.output_path = UniqueTempPath("trace_runner_smoke");
  opts.budget.instructions = 5;

  const auto result = pupsnes::tools::RunTrace(opts);

  REQUIRE(result.ok);
  REQUIRE(result.error.empty());
  REQUIRE(result.instructions_emitted == 5);

  const auto lines = ReadAllLines(opts.output_path);
  REQUIRE_FALSE(lines.empty());
  REQUIRE(lines.front().rfind("# pupsnes-trace v1", 0) == 0);

  // Header + 5 instructions = 6 lines.
  REQUIRE(lines.size() == 6U);

  // First instruction line must show the reset-vector PC ($8000) and the
  // opcode byte for `LDA #$12` ($A9).
  REQUIRE(lines[1].rfind("00:8000", 0) == 0);
  REQUIRE(lines[1].find(" A9 ") != std::string::npos);
}

TEST_CASE("RunTrace returns ok=false when the ROM path does not exist", "[unit][trace_runner]") {
  pupsnes::tools::TraceRunOptions opts;
  opts.rom_path = "/no/such/rom.sfc";
  opts.output_path = UniqueTempPath("trace_runner_missing");
  opts.budget.instructions = 1;

  const auto result = pupsnes::tools::RunTrace(opts);

  REQUIRE_FALSE(result.ok);
  REQUIRE_FALSE(result.error.empty());
}

TEST_CASE("RunTrace stops at the master-cycle budget", "[unit][trace_runner]") {
  pupsnes::tools::TraceRunOptions opts;
  opts.rom_path = TestRomPath("instruction_smoke");
  opts.output_path = UniqueTempPath("trace_runner_master_cycles");
  opts.budget.master_cycles = 200;  // tiny budget — only a handful of insts

  const auto result = pupsnes::tools::RunTrace(opts);

  REQUIRE(result.ok);
  REQUIRE(result.master_time_elapsed >= 200);
  REQUIRE(result.instructions_emitted >= 1);
}

TEST_CASE("DriveMachineToMasterTime reports SPC700 faults during catch-up", "[unit][trace_runner][apu]") {
  pupsnes::SNES snes;
  std::array<uint8_t, pupsnes::Cartridge::kLoROMWindowSize> rom{};
  rom[0] = 0x80;  // BRA self: no CPU access to the APU before MachineSync.
  rom[1] = 0xFE;
  rom[0x7FFC] = 0x00;
  rom[0x7FFD] = 0x80;
  REQUIRE(snes.LoadRom(rom).ok);
  snes.Reset();

  pupsnes::Spc700::State state;
  state.pc = 0x0200;
  snes.GetApu().GetCpu().Reset(state);
  snes.GetApu().Write(0x0200, 0xE4);  // MOV A,$F3: unsupported DSP data access.
  snes.GetApu().Write(0x0201, 0xF3);

  const auto error = pupsnes::tools::DriveMachineToMasterTime(snes, 1000);
  REQUIRE(error.has_value());
  const auto message = error.value_or("");
  REQUIRE(message.find("Emulation exception: ") == 0);
  REQUIRE(message.find("SPC700") != std::string::npos);
  REQUIRE(message.find("register $00F3") != std::string::npos);
  REQUIRE(message.find("$0202") != std::string::npos);
  REQUIRE_FALSE(snes.GetCpu().GetFault().has_value());
}
