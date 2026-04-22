#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>

#include "pupsnes/debugger/breakpoints.h"
#include "pupsnes/debugger/error_log.h"
#include "pupsnes/debugger/run_control.h"
#include "pupsnes/debugger/trace.h"
#include "pupsnes/hw/cartridge.h"
#include "pupsnes/hw/snes.h"

using namespace pupsnes;            // NOLINT(google-build-using-namespace)
using namespace pupsnes::debugger;  // NOLINT(google-build-using-namespace)

namespace {

struct DebuggerFixture {
  SNES snes;
  std::array<uint8_t, Cartridge::kLoROMWindowSize> rom{};
  BreakpointSet breakpoints;
  TraceLog trace;
  ErrorLog errors;

  DebuggerFixture() : trace(32), errors(32) {
    rom.fill(0xEA);
    rom[0x7FFC] = 0x00;
    rom[0x7FFD] = 0x80;
  }

  void SetBytes(std::initializer_list<uint8_t> bytes) {
    std::size_t offset = 0;
    for (uint8_t byte : bytes) {
      rom[offset++] = byte;
    }
  }

  RunControl BuildRunControl() {
    snes.LoadLoRom(rom);
    snes.Reset();
    return RunControl(snes, breakpoints, trace, errors);
  }
};

}  // namespace

TEST_CASE("RunControl StepOne retires exactly one instruction", "[unit][debugger]") {
  DebuggerFixture fixture;
  fixture.SetBytes({0xA9, 0x11, 0xEA, 0xA9, 0x22});

  RunControl run_control = fixture.BuildRunControl();
  run_control.RequestStepOne();
  run_control.TickFrame(std::chrono::seconds(1));

  REQUIRE(run_control.GetState() == RunState::kPaused);
  REQUIRE(run_control.GetPauseReason() == PauseReason::kUser);
  REQUIRE(fixture.snes.GetCpu().GetRetiredInstructionCount() == 1);
  REQUIRE(fixture.snes.GetCpu().GetRegs().PC == 0x8002);
  REQUIRE(fixture.trace.Size() == 1);
}

TEST_CASE("RunControl StepOne after Pause retires one instruction", "[unit][debugger]") {
  DebuggerFixture fixture;
  // BRA self: a 0x80 0xFE tight loop so Run can execute indefinitely without
  // hitting undefined instructions / faults. 0xEA (NOP) in every other cell
  // is never executed; present only so the reset vector still lands here.
  fixture.SetBytes({0x80, 0xFE});

  RunControl run_control = fixture.BuildRunControl();
  run_control.RequestRunUntilBreak();
  run_control.TickFrame(std::chrono::milliseconds(5));
  run_control.Pause();
  REQUIRE(run_control.GetState() == RunState::kPaused);
  REQUIRE(run_control.GetPauseReason() == PauseReason::kUser);
  const uint64_t retired_at_pause = fixture.snes.GetCpu().GetRetiredInstructionCount();

  run_control.RequestStepOne();
  run_control.TickFrame(std::chrono::seconds(1));

  REQUIRE(run_control.GetState() == RunState::kPaused);
  REQUIRE(run_control.GetPauseReason() == PauseReason::kUser);
  REQUIRE(fixture.snes.GetCpu().GetRetiredInstructionCount() == retired_at_pause + 1);
}

TEST_CASE("RunControl StepOne after a breakpoint pause retires one instruction", "[unit][debugger]") {
  DebuggerFixture fixture;
  fixture.SetBytes({0xEA, 0xEA, 0xEA, 0xEA, 0xEA});
  fixture.breakpoints.Set(0x008002);

  RunControl run_control = fixture.BuildRunControl();
  run_control.RequestRunUntilBreak();
  run_control.TickFrame(std::chrono::seconds(1));
  REQUIRE(run_control.GetState() == RunState::kPaused);
  REQUIRE(run_control.GetPauseReason() == PauseReason::kBreakpoint);
  REQUIRE(fixture.snes.GetCpu().GetRegs().PC == 0x8002);
  const uint64_t retired_at_pause = fixture.snes.GetCpu().GetRetiredInstructionCount();

  run_control.RequestStepOne();
  run_control.TickFrame(std::chrono::seconds(1));

  REQUIRE(run_control.GetState() == RunState::kPaused);
  REQUIRE(run_control.GetPauseReason() == PauseReason::kUser);
  REQUIRE(fixture.snes.GetCpu().GetRetiredInstructionCount() == retired_at_pause + 1);
  REQUIRE(fixture.snes.GetCpu().GetRegs().PC == 0x8003);
}

TEST_CASE("RunControl StepN stops after N instruction boundaries", "[unit][debugger]") {
  DebuggerFixture fixture;
  fixture.SetBytes({0xA9, 0x11, 0xEA, 0xA9, 0x22});

  RunControl run_control = fixture.BuildRunControl();
  run_control.RequestStepN(2);
  run_control.TickFrame(std::chrono::seconds(1));

  REQUIRE(run_control.GetState() == RunState::kPaused);
  REQUIRE(run_control.GetPauseReason() == PauseReason::kUser);
  REQUIRE(fixture.snes.GetCpu().GetRetiredInstructionCount() == 2);
  REQUIRE(fixture.snes.GetCpu().GetRegs().PC == 0x8003);
  REQUIRE(static_cast<uint8_t>(fixture.snes.GetCpu().GetRegs().A) == 0x11);
  REQUIRE(fixture.trace.Size() == 2);
}

TEST_CASE("RunControl RunUntilBreak halts on a breakpoint hit", "[unit][debugger]") {
  DebuggerFixture fixture;
  fixture.SetBytes({0xEA, 0xEA, 0xEA});
  fixture.breakpoints.Set(0x008001);

  RunControl run_control = fixture.BuildRunControl();
  run_control.RequestRunUntilBreak();
  run_control.TickFrame(std::chrono::seconds(1));

  REQUIRE(run_control.GetState() == RunState::kPaused);
  REQUIRE(run_control.GetPauseReason() == PauseReason::kBreakpoint);
  REQUIRE(fixture.snes.GetCpu().GetRegs().PC == 0x8001);
  REQUIRE(fixture.trace.Size() == 1);
  REQUIRE(fixture.errors.Snapshot().empty());
}

TEST_CASE("RunControl StepOne microop stops after one bus access", "[unit][debugger]") {
  DebuggerFixture fixture;
  fixture.SetBytes({0xA9, 0x11, 0xEA});  // LDA #$11 ; NOP

  RunControl run_control = fixture.BuildRunControl();
  run_control.RequestStepOne(StepGranularity::kMicroOp);
  run_control.TickFrame(std::chrono::seconds(1));

  REQUIRE(run_control.GetState() == RunState::kPaused);
  REQUIRE(fixture.snes.GetCpu().GetRetiredInstructionCount() == 0);
}

TEST_CASE("RunControl RunUntilBreak halts on CPU fault and logs it", "[unit][debugger]") {
  DebuggerFixture fixture;
  fixture.SetBytes({0xEA, 0x00, 0xEA});

  RunControl run_control = fixture.BuildRunControl();
  run_control.RequestRunUntilBreak();
  run_control.TickFrame(std::chrono::seconds(1));

  REQUIRE(run_control.GetState() == RunState::kPaused);
  REQUIRE(run_control.GetPauseReason() == PauseReason::kError);
  REQUIRE(fixture.trace.Size() == 1);

  const auto errors = fixture.errors.Snapshot();
  REQUIRE(errors.size() == 1);
  REQUIRE(errors.front().source == ErrorSource::kCpu);
  REQUIRE(errors.front().address == 0x008001);
}
