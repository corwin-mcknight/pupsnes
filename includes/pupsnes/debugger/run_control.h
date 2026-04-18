#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "pupsnes/debugger/breakpoints.h"
#include "pupsnes/debugger/error_log.h"
#include "pupsnes/debugger/trace.h"
#include "pupsnes/hw/snes.h"

namespace pupsnes::debugger {

enum class RunState : uint8_t {
  kPaused = 0,
  kStepOne = 1,
  kStepN = 2,
  kRunUntilBreak = 3,
};

enum class PauseReason : uint8_t {
  kUser = 0,
  kBreakpoint = 1,
  kError = 2,
};

class RunControl {
 public:
  RunControl(SNES& snes, BreakpointSet& breakpoints, TraceLog& trace_log, ErrorLog& error_log);

  void ResetMachineState();
  void Pause();
  void RequestStepOne();
  void RequestStepN(uint64_t count);
  void RequestRunUntilBreak();
  void TickFrame(std::chrono::steady_clock::duration wall_clock_budget);

  [[nodiscard]] RunState GetState() const { return state_; }
  [[nodiscard]] PauseReason GetPauseReason() const { return pause_reason_; }
  [[nodiscard]] uint64_t GetRemainingSteps() const { return remaining_steps_; }

 private:
  [[nodiscard]] SnesAddrT GetCurrentPc() const;
  void PrimeCpuRun();
  [[nodiscard]] bool ShouldPauseOnCurrentPc() const;
  void PauseForBreakpoint();
  void PauseForError();
  bool RunSingleInstructionBoundary();
  void RecordInstructionTrace(SnesAddrT pc_before, const CPU::Regs& regs_before);
  void LogFaultIfPresent();

  SNES& snes_;
  BreakpointSet& breakpoints_;
  TraceLog& trace_log_;
  ErrorLog& error_log_;
  RunState state_ = RunState::kPaused;
  PauseReason pause_reason_ = PauseReason::kUser;
  uint64_t remaining_steps_ = 0;
  std::optional<SnesAddrT> suppressed_breakpoint_ = std::nullopt;
  std::optional<SnesAddrT> logged_fault_pc_ = std::nullopt;
  MicroOpRecorder* saved_microop_recorder_ = nullptr;

  void DetachMicroOpRecorderForFreeRun();
  void RestoreMicroOpRecorderForPause();
};

}  // namespace pupsnes::debugger
