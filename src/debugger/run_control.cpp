#include "pupsnes/debugger/run_control.h"

#include <exception>

#include "pupsnes/core/scheduler.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/5a22/micro_op.h"

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
  Pause();
  logged_fault_pc_.reset();
  InstallDebuggerContract();
}

void RunControl::Pause() {
  state_ = RunState::kPaused;
  pause_reason_ = PauseReason::kUser;
  snes_.GetCpu().MutableDebuggerContract().step_target = 0;
  RestoreMicroOpRecorder();
}

void RunControl::DetachMicroOpRecorderForFreeRun() {
  MicroOpRecorder* current = snes_.GetCpu().GetMicroOpRecorder();
  if (current == nullptr) {
    return;
  }
  current->OnRecordingInterrupted();
  saved_microop_recorder_ = current;
  snes_.GetCpu().SetMicroOpRecorder(nullptr);
}

void RunControl::RestoreMicroOpRecorder() {
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
  RequestStepN(1, granularity);
  state_ = RunState::kStepOne;
}

void RunControl::RequestStepN(uint64_t count, StepGranularity granularity) {
  if (count == 0) {
    Pause();
    return;
  }
  RestoreMicroOpRecorder();
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
  Pause();
  pause_reason_ = PauseReason::kBreakpoint;
}

void RunControl::PauseForError() {
  Pause();
  pause_reason_ = PauseReason::kError;
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
      // Passive devices can fault during catch-up (for example, when the
      // SPC700 encounters an instruction that has not been implemented).
      // Keep the whole machine advance inside the debugger error boundary.
      const TimeMasterT now = snes_.GetMasterTime();
      snes_.MachineSync(now);
      snes_.GetScheduler().FireEventsThrough(now);
    } catch (const std::exception& ex) {
      error_log_.PushSchedulerError(snes_.GetMasterTime(), ex.what(), snes_.GetCpu().GetRegs());
      PauseForError();
      break;
    }

    if (!HandlePostTickState(result)) {
      break;
    }
  }
}

}  // namespace pupsnes::debugger
