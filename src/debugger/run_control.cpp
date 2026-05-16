#include "pupsnes/debugger/run_control.h"

#include <exception>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/core/scheduler.h"

namespace pupsnes::debugger {

namespace {

SnesAddrT ComposePcAddress(const CPU::Regs& regs) {
  return (static_cast<SnesAddrT>(regs.PBR) << 16U) | static_cast<SnesAddrT>(regs.PC);
}

}  // namespace

RunControl::RunControl(SNES& snes, BreakpointSet& breakpoints, TraceSink& trace_sink, ErrorLog& error_log)
    : snes_(snes), breakpoints_(breakpoints), trace_sink_(trace_sink), error_log_(error_log) {
  ResetMachineState();
}

void RunControl::InstallDebuggerContract() {
  DebuggerContract contract;
  contract.breakpoints = &breakpoints_;
  contract.trace_sink = &trace_sink_;
  contract.step_target = 0;
  contract.suppressed_breakpoint_pc = std::nullopt;
  contract.step_granularity = StepGranularity::kInstruction;
  snes_.GetCpu().SetDebuggerContract(contract);
}

void RunControl::ResetMachineState() {
  state_ = RunState::kPaused;
  pause_reason_ = PauseReason::kUser;
  logged_fault_pc_.reset();
  InstallDebuggerContract();
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

void RunControl::RequestStepOne(StepGranularity granularity) {
  SuppressBreakpointAtCurrentPc();
  auto& c = snes_.GetCpu().MutableDebuggerContract();
  c.step_target = 1;
  c.step_granularity = granularity;
  state_ = RunState::kStepOne;
}

void RunControl::RequestStepN(uint64_t count, StepGranularity granularity) {
  if (count == 0) {
    state_ = RunState::kPaused;
    snes_.GetCpu().MutableDebuggerContract().step_target = 0;
    return;
  }
  SuppressBreakpointAtCurrentPc();
  auto& c = snes_.GetCpu().MutableDebuggerContract();
  c.step_target = count;
  c.step_granularity = granularity;
  state_ = RunState::kStepN;
}

void RunControl::RequestRunUntilBreak() {
  SuppressBreakpointAtCurrentPc();
  auto& c = snes_.GetCpu().MutableDebuggerContract();
  c.step_target = 0;
  c.step_granularity = StepGranularity::kInstruction;
  state_ = RunState::kRunUntilBreak;
  DetachMicroOpRecorderForFreeRun();
}

SnesAddrT RunControl::GetCurrentPc() const { return ComposePcAddress(snes_.GetCpu().GetRegs()); }

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

bool RunControl::HandlePostTickState(const TickResult& result) {
  CPU& cpu = snes_.GetCpu();

  if (cpu.GetFault().has_value()) {
    LogFaultIfPresent();
    PauseForError();
    return false;
  }

  switch (result.reason) {
    case TickStopReason::kBreakpoint: PauseForBreakpoint(); return false;
    case TickStopReason::kRetiredStepTarget: Pause(); return false;
    case TickStopReason::kFault: PauseForError(); return false;
    case TickStopReason::kReachedTarget: return true;
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
    if (master_cycles_budget.has_value() && (snes_.GetMasterTime() - start_master_time) >= *master_cycles_budget) {
      break;
    }

    TimeMasterT target = snes_.GetScheduler().NextEventMasterTime();
    // Clamp to the cycle budget so the CPU doesn't overshoot into the next
    // scheduler event. Without this clamp, a TickToTarget call would advance
    // all the way to the next kFrameEnd (~357K master cycles), blowing past
    // smaller budgets and producing ~200% effective speed at >60Hz hosts.
    if (master_cycles_budget.has_value()) {
      const TimeMasterT budget_target = start_master_time + *master_cycles_budget;
      if (budget_target < target) {
        target = budget_target;
      }
    }

    TickResult result{0, TickStopReason::kReachedTarget};
    try {
      result = snes_.GetCpu().TickToTarget(target);
    } catch (const std::exception& ex) {
      error_log_.PushSchedulerError(snes_.GetMasterTime(), ex.what(), snes_.GetCpu().GetRegs());
      PauseForError();
      break;
    }

    const TimeMasterT now = snes_.GetMasterTime();
    snes_.MachineSync(now);
    snes_.GetScheduler().FireEventsThrough(now);

    if (!HandlePostTickState(result)) {
      break;
    }
  }
}

}  // namespace pupsnes::debugger
