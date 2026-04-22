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

void RunControl::InstallDebuggerContract() {
  DebuggerContract contract;
  contract.breakpoints = &breakpoints_;
  contract.trace_sink = &trace_log_;
  contract.step_target = 0;
  contract.suppressed_breakpoint_pc = std::nullopt;
  snes_.GetCpu().SetDebuggerContract(contract);
  (void)snes_.GetCpu().TakeLastDebuggerStop();
}

void RunControl::ResetMachineState() {
  state_ = RunState::kPaused;
  pause_reason_ = PauseReason::kUser;
  logged_fault_pc_.reset();
  InstallDebuggerContract();
  PrimeCpuRun();
}

void RunControl::Pause() {
  state_ = RunState::kPaused;
  pause_reason_ = PauseReason::kUser;
  snes_.GetCpu().MutableDebuggerContract().step_target = 0;
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

void RunControl::SuppressBreakpointAtCurrentPc() {
  const SnesAddrT current_pc = GetCurrentPc();
  DebuggerContract& contract = snes_.GetCpu().MutableDebuggerContract();
  if (breakpoints_.IsEnabled(current_pc)) {
    contract.suppressed_breakpoint_pc = current_pc;
  } else {
    contract.suppressed_breakpoint_pc.reset();
  }
}

void RunControl::RequestStepOne() {
  SuppressBreakpointAtCurrentPc();
  snes_.GetCpu().MutableDebuggerContract().step_target = 1;
  state_ = RunState::kStepOne;
  PrimeCpuRun();
}

void RunControl::RequestStepN(uint64_t count) {
  if (count == 0) {
    state_ = RunState::kPaused;
    snes_.GetCpu().MutableDebuggerContract().step_target = 0;
    return;
  }
  SuppressBreakpointAtCurrentPc();
  snes_.GetCpu().MutableDebuggerContract().step_target = count;
  state_ = RunState::kStepN;
  PrimeCpuRun();
}

void RunControl::RequestRunUntilBreak() {
  SuppressBreakpointAtCurrentPc();
  snes_.GetCpu().MutableDebuggerContract().step_target = 0;
  state_ = RunState::kRunUntilBreak;
  DetachMicroOpRecorderForFreeRun();
  PrimeCpuRun();
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

void RunControl::PauseForBreakpoint() {
  state_ = RunState::kPaused;
  pause_reason_ = PauseReason::kBreakpoint;
  snes_.GetCpu().MutableDebuggerContract().step_target = 0;
  RestoreMicroOpRecorderForPause();
}

void RunControl::PauseForError() {
  state_ = RunState::kPaused;
  pause_reason_ = PauseReason::kError;
  snes_.GetCpu().MutableDebuggerContract().step_target = 0;
  RestoreMicroOpRecorderForPause();
}

void RunControl::LogFaultIfPresent() {
  const std::optional<CPU::Fault>& fault = snes_.GetCpu().GetFault();
  if (!fault.has_value() || logged_fault_pc_ == fault->opcode_address) {
    return;
  }

  logged_fault_pc_ = fault->opcode_address;
  error_log_.PushCpuFault(snes_.GetMasterTime(), *fault);
}

bool RunControl::HandlePostStepState() {
  CPU& cpu = snes_.GetCpu();

  if (cpu.GetFault().has_value()) {
    LogFaultIfPresent();
    PauseForError();
    return false;
  }

  if (const auto stop = cpu.TakeLastDebuggerStop(); stop.has_value()) {
    switch (*stop) {
      case TickStopReason::kDebuggerBreakpoint: PauseForBreakpoint(); return false;
      case TickStopReason::kDebuggerStepComplete: Pause(); return false;
      default: break;
    }
  }

  return true;
}

void RunControl::TickFrame(std::chrono::steady_clock::duration wall_clock_budget,
                           std::optional<TimeMasterT> master_cycles_budget) {
  if (state_ == RunState::kPaused) {
    return;
  }

  logged_fault_pc_.reset();
  const auto deadline = std::chrono::steady_clock::now() + wall_clock_budget;
  const TimeMasterT start_master_time = snes_.GetMasterTime();

  while (state_ != RunState::kPaused && std::chrono::steady_clock::now() < deadline) {
    if (master_cycles_budget.has_value() &&
        (snes_.GetMasterTime() - start_master_time) >= *master_cycles_budget) {
      break;
    }
    if (!snes_.GetScheduler().HasPendingEvents()) {
      Pause();
      break;
    }

    try {
      snes_.GetScheduler().Step();
    } catch (const std::exception& ex) {
      error_log_.PushSchedulerError(snes_.GetMasterTime(), ex.what(), snes_.GetCpu().GetRegs());
      PauseForError();
      break;
    }

    if (!HandlePostStepState()) {
      break;
    }
  }
}

}  // namespace pupsnes::debugger
