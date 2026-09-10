#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>

#include "pupsnes/core/scheduler.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/debugger/breakpoints.h"
#include "pupsnes/debugger/error_log.h"
#include "pupsnes/debugger/fan_out_trace_sink.h"
#include "pupsnes/debugger/run_control.h"
#include "pupsnes/debugger/trace.h"
#include "pupsnes/hw/apu/apu.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/hw/sppu/ppu_regs.h"

using namespace pupsnes;            // NOLINT(google-build-using-namespace)
using namespace pupsnes::debugger;  // NOLINT(google-build-using-namespace)

namespace {

struct DebuggerFixture {
  SNES snes;
  std::array<uint8_t, Cartridge::kLoROMWindowSize> rom{};
  BreakpointSet breakpoints;
  TraceLog trace;
  FanOutTraceSink fan_out;
  ErrorLog errors;

  DebuggerFixture() : trace(32), errors(32) {
    rom.fill(0xEA);
    rom[0x7FFC] = 0x00;
    rom[0x7FFD] = 0x80;
    fan_out.Attach(&trace);
  }

  void SetBytes(std::initializer_list<uint8_t> bytes) {
    std::size_t offset = 0;
    for (uint8_t byte : bytes) {
      rom[offset++] = byte;
    }
  }

  RunControl BuildRunControl() {
    snes.LoadRom(rom);
    snes.Reset();
    return RunControl(snes, breakpoints, fan_out, errors);
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
  fixture.SetBytes({0xEA, 0xEA, 0xEA});

  RunControl run_control = fixture.BuildRunControl();

  // Retire the first NOP so the trace has one entry, matching the pre-fault
  // execution that used to come from the unimplemented-opcode path.
  run_control.RequestStepOne();
  run_control.TickFrame(std::chrono::seconds(1));
  REQUIRE(fixture.trace.Size() == 1);

  // Inject a CPU fault at the PC of the next instruction. Every 65C816 opcode
  // is implemented, so the only way to drive the fault-handling path is via
  // the test-only injection API.
  fixture.snes.GetCpu().DebugInjectFault(0xFF, 0x008001);

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

TEST_CASE("RunControl pauses and logs SPC700 faults raised during MachineSync", "[unit][debugger][apu]") {
  DebuggerFixture fixture;
  // The main CPU never accesses APU ports, so the SPC700 first advances in
  // MachineSync after TickToTarget returns.
  fixture.SetBytes({0x80, 0xFE});
  RunControl run_control = fixture.BuildRunControl();

  Spc700::State state{};
  state.pc = 0x0200;
  fixture.snes.GetApu().GetCpu().Reset(state);
  fixture.snes.GetApu().Write(0x0200, 0xE4);  // MOV A,$F3: unsupported DSP data access.
  fixture.snes.GetApu().Write(0x0201, 0xF3);

  run_control.RequestRunUntilBreak();
  REQUIRE_NOTHROW(run_control.TickFrame(std::chrono::seconds(1), 1000));

  REQUIRE(run_control.GetState() == RunState::kPaused);
  REQUIRE(run_control.GetPauseReason() == PauseReason::kError);
  REQUIRE(fixture.snes.GetCpu().GetRetiredInstructionCount() > 0);
  REQUIRE_FALSE(fixture.snes.GetCpu().GetFault().has_value());
  const auto errors = fixture.errors.Snapshot();
  REQUIRE(errors.size() == 1);
  REQUIRE(errors.front().message.find("SPC700") != std::string::npos);
  REQUIRE(errors.front().message.find("register $00F3") != std::string::npos);
  REQUIRE(errors.front().message.find("0202") != std::string::npos);
}

TEST_CASE("RunControl tight BRA loop advances past kFrameEnd boundary", "[unit][debugger]") {
  // Regression: strict-no-overshoot in TickToTarget could leave master_time
  // parked a few cycles before a scheduled event (next_cost > remaining), so
  // the outer TickFrame loop spun indefinitely with zero CPU progress and the
  // kFrameEnd event never fired. This test verifies that a ROM in a tight BRA
  // self-loop advances master_time past at least one full frame boundary.
  DebuggerFixture fixture;
  // BRA self ($80 $FE) — infinite 2-byte relative branch, ~8 master cycles per
  // iteration. Exercises the exact scenario: the CPU stops just before a
  // kFrameEnd event because the next BRA would overshoot, but master_time must
  // still advance to the event boundary so the event can fire.
  fixture.SetBytes({0x80, 0xFE});

  RunControl run_control = fixture.BuildRunControl();
  run_control.RequestRunUntilBreak();

  const TimeMasterT one_frame = 262U * sppu::regs::kNormalLineCycles;

  // Give a generous wall-clock budget. TickFrame must advance past one frame
  // boundary — without the fix it would spin forever emitting no PPU frames.
  run_control.TickFrame(std::chrono::milliseconds(100),
                        /*master_cycles_budget=*/one_frame * 3);

  // CPU must have advanced beyond the first kFrameEnd boundary.
  REQUIRE(fixture.snes.GetMasterTime() > one_frame);

  // Scheduler must have re-queued the next kFrameEnd (not stuck on the first).
  const auto events = fixture.snes.GetScheduler().SnapshotSignalQueue();
  REQUIRE(!events.empty());
  REQUIRE(events.front().master_time > one_frame);
}
