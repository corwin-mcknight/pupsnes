#include <unistd.h>  // getpid

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "pupsnes/debugger/breakpoints.h"
#include "pupsnes/debugger/error_log.h"
#include "pupsnes/debugger/fan_out_trace_sink.h"
#include "pupsnes/debugger/file_trace_sink.h"
#include "pupsnes/debugger/run_control.h"
#include "pupsnes/debugger/sha1.h"
#include "pupsnes/debugger/trace.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/core/snes.h"

using namespace pupsnes;            // NOLINT
using namespace pupsnes::debugger;  // NOLINT

namespace {

std::string ReadAll(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::filesystem::path TempTracePath(const char* tag) {
  return std::filesystem::temp_directory_path() /
         (std::string("pupsnes_trace_det_") + tag + "_" +
          std::to_string(static_cast<int64_t>(::getpid())) + ".log");
}

struct DeterminismFixture {
  SNES snes;
  std::array<uint8_t, Cartridge::kLoROMWindowSize> rom{};
  BreakpointSet breakpoints;
  TraceLog trace;
  FanOutTraceSink fan_out;
  ErrorLog errors;
  Sha1Digest rom_sha1{};

  DeterminismFixture() : trace(32), errors(32) {
    rom.fill(0xEA);
    rom[0x7FFC] = 0x00;
    rom[0x7FFD] = 0x80;
    fan_out.Attach(&trace);

    Sha1 hasher;
    hasher.Update(rom.data(), rom.size());
    rom_sha1 = hasher.Finalize();
  }

  void ResetMachine() {
    snes.LoadRom(rom);
    snes.Reset();
  }
};

}  // namespace

TEST_CASE("Trace file is byte-identical across two cold-boot step runs", "[unit][debugger]") {
  const auto path_a = TempTracePath("a");
  const auto path_b = TempTracePath("b");
  std::error_code ec;

  {
    DeterminismFixture fx;
    fx.ResetMachine();
    RunControl run_control(fx.snes, fx.breakpoints, fx.fan_out, fx.errors);

    FileTraceSink file_sink(fx.snes, path_a.string(), fx.rom_sha1);
    REQUIRE(!file_sink.HasError());
    fx.fan_out.Attach(&file_sink);

    run_control.RequestStepN(100);
    run_control.TickFrame(std::chrono::seconds(1));
    fx.fan_out.Detach(&file_sink);
  }
  {
    DeterminismFixture fx;
    fx.ResetMachine();
    RunControl run_control(fx.snes, fx.breakpoints, fx.fan_out, fx.errors);

    FileTraceSink file_sink(fx.snes, path_b.string(), fx.rom_sha1);
    REQUIRE(!file_sink.HasError());
    fx.fan_out.Attach(&file_sink);

    run_control.RequestStepN(100);
    run_control.TickFrame(std::chrono::seconds(1));
    fx.fan_out.Detach(&file_sink);
  }

  const std::string a = ReadAll(path_a);
  const std::string b = ReadAll(path_b);

  CHECK(!a.empty());
  CHECK(a == b);

  std::filesystem::remove(path_a, ec);
  std::filesystem::remove(path_b, ec);
}

TEST_CASE("Trace file contains expected number of body lines for StepN", "[unit][debugger]") {
  const auto path = TempTracePath("count");
  std::error_code ec;

  {
    DeterminismFixture fx;
    fx.ResetMachine();
    RunControl run_control(fx.snes, fx.breakpoints, fx.fan_out, fx.errors);

    FileTraceSink file_sink(fx.snes, path.string(), fx.rom_sha1);
    fx.fan_out.Attach(&file_sink);
    run_control.RequestStepN(10);
    run_control.TickFrame(std::chrono::seconds(1));
    fx.fan_out.Detach(&file_sink);
    CHECK(file_sink.LineCount() == 10);
  }

  std::filesystem::remove(path, ec);
}
