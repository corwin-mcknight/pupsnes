#include "pupsnes/debugger/run_control.h"

#include <algorithm>
#include <exception>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/scheduler.h"

namespace pupsnes::debugger {

namespace {

SnesAddrT ComposePcAddress(const CPU::Regs& regs) {
  return (static_cast<SnesAddrT>(regs.PBR) << 16U) | static_cast<SnesAddrT>(regs.PC);
}

}  // namespace

RunControl::RunControl(SNES& snes, BreakpointSet& breakpoints, TraceLog& trace_log, ErrorLog& error_log)
    : snes_(snes), breakpoints_(breakpoints), trace_log_(trace_log), error_log_(error_log) {
  ResetMachineState();
}

void RunControl::ResetMachineState() {
  state_ = RunState::kPaused;
  pause_reason_ = PauseReason::kUser;
  remaining_steps_ = 0;
  suppressed_breakpoint_.reset();
  logged_fault_pc_.reset();
  snes_.GetCpu().SetBoundaryStopEnabled(true);
  PrimeCpuRun();
}

void RunControl::Pause() {
  state_ = RunState::kPaused;
  pause_reason_ = PauseReason::kUser;
  remaining_steps_ = 0;
  RestoreMicroOpRecorderForPause();
}

void RunControl::DetachMicroOpRecorderForFreeRun() {
  MicroOpRecorder* current = snes_.GetCpu().GetMicroOpRecorder();
  if (current == nullptr) {
    return;
  }
  saved_microop_recorder_ = current;
  snes_.GetCpu().SetMicroOpRecorder(nullptr);
}

void RunControl::RestoreMicroOpRecorderForPause() {
  if (saved_microop_recorder_ == nullptr) {
    return;
  }
  snes_.GetCpu().SetMicroOpRecorder(saved_microop_recorder_);
  saved_microop_recorder_ = nullptr;
}

void RunControl::RequestStepOne() {
  const SnesAddrT current_pc = GetCurrentPc();
  if (breakpoints_.IsEnabled(current_pc)) {
    suppressed_breakpoint_ = current_pc;
  }
  remaining_steps_ = 1;
  state_ = RunState::kStepOne;
}

void RunControl::RequestStepN(uint64_t count) {
  const SnesAddrT current_pc = GetCurrentPc();
  if (count > 0 && breakpoints_.IsEnabled(current_pc)) {
    suppressed_breakpoint_ = current_pc;
  }
  remaining_steps_ = count;
  state_ = (count == 0) ? RunState::kPaused : RunState::kStepN;
}

void RunControl::RequestRunUntilBreak() {
  state_ = RunState::kRunUntilBreak;
  DetachMicroOpRecorderForFreeRun();
}

SnesAddrT RunControl::GetCurrentPc() const { return ComposePcAddress(snes_.GetCpu().GetRegs()); }

void RunControl::PrimeCpuRun() {
  const TimeMasterT next_time = std::max(snes_.GetMasterTime(), snes_.GetCpu().GetTime());
  CPU& cpu = snes_.GetCpu();
  if (snes_.GetScheduler().HasPendingRunAtOrBefore(cpu.GetDeviceId(), next_time)) {
    return;
  }
  snes_.GetScheduler().ScheduleDeviceRun(&cpu, next_time);
}

bool RunControl::ShouldPauseOnCurrentPc() const {
  const SnesAddrT current_pc = GetCurrentPc();
  return breakpoints_.IsEnabled(current_pc) && suppressed_breakpoint_ != current_pc;
}

void RunControl::PauseForBreakpoint() {
  state_ = RunState::kPaused;
  pause_reason_ = PauseReason::kBreakpoint;
  remaining_steps_ = 0;
  RestoreMicroOpRecorderForPause();
}

void RunControl::PauseForError() {
  state_ = RunState::kPaused;
  pause_reason_ = PauseReason::kError;
  remaining_steps_ = 0;
  RestoreMicroOpRecorderForPause();
}

void RunControl::RecordInstructionTrace(SnesAddrT pc_before, const CPU::Regs& regs_before) {
  trace_log_.Push({
      .master_time = snes_.GetMasterTime(),
      .pc = pc_before,
      .regs = regs_before,
  });
}

void RunControl::LogFaultIfPresent() {
  const std::optional<CPU::Fault>& fault = snes_.GetCpu().GetFault();
  if (!fault.has_value() || logged_fault_pc_ == fault->opcode_address) {
    return;
  }

  logged_fault_pc_ = fault->opcode_address;
  error_log_.PushCpuFault(snes_.GetMasterTime(), *fault);
}

bool RunControl::RunSingleInstructionBoundary() {
  PrimeCpuRun();

  if (ShouldPauseOnCurrentPc()) {
    PauseForBreakpoint();
    return false;
  }

  CPU& cpu = snes_.GetCpu();
  const CPU::Regs regs_before = cpu.GetRegs();
  const SnesAddrT pc_before = ComposePcAddress(regs_before);
  const uint64_t retired_before = cpu.GetRetiredInstructionCount();

  while (cpu.GetRetiredInstructionCount() == retired_before) {
    try {
      snes_.GetScheduler().Step();
    } catch (const std::exception& ex) {
      error_log_.PushSchedulerError(snes_.GetMasterTime(), ex.what(), cpu.GetRegs());
      PauseForError();
      return false;
    }

    if (cpu.GetFault().has_value()) {
      LogFaultIfPresent();
      PauseForError();
      return false;
    }

    if (!snes_.GetScheduler().HasPendingEvents()) {
      break;
    }
  }

  if (cpu.GetRetiredInstructionCount() == retired_before) {
    return false;
  }

  suppressed_breakpoint_.reset();
  RecordInstructionTrace(pc_before, regs_before);
  logged_fault_pc_.reset();

  if (breakpoints_.IsEnabled(GetCurrentPc())) {
    PauseForBreakpoint();
    return false;
  }

  return true;
}

void RunControl::TickFrame(std::chrono::steady_clock::duration wall_clock_budget) {
  if (state_ == RunState::kPaused) {
    return;
  }

  const auto deadline = std::chrono::steady_clock::now() + wall_clock_budget;
  while (state_ != RunState::kPaused && std::chrono::steady_clock::now() < deadline) {
    const bool advanced = RunSingleInstructionBoundary();

    if (state_ == RunState::kPaused) {
      break;
    }
    if (!advanced) {
      Pause();
      break;
    }

    switch (state_) {
      case RunState::kPaused:
        break;
      case RunState::kStepOne:
        Pause();
        break;
      case RunState::kStepN:
        if (remaining_steps_ > 0) {
          --remaining_steps_;
        }
        if (remaining_steps_ == 0) {
          Pause();
        }
        break;
      case RunState::kRunUntilBreak:
        break;
    }
  }
}

}  // namespace pupsnes::debugger
