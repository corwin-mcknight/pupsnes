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
  void TickFrame(std::chrono::steady_clock::duration wall_clock_budget,
                 std::optional<TimeMasterT> master_cycles_budget = std::nullopt);

  [[nodiscard]] RunState GetState() const { return state_; }
  [[nodiscard]] PauseReason GetPauseReason() const { return pause_reason_; }

 private:
  [[nodiscard]] SnesAddrT GetCurrentPc() const;
  void PrimeCpuRun();
  void InstallDebuggerContract();
  void SuppressBreakpointAtCurrentPc();
  void PauseForBreakpoint();
  void PauseForError();
  void LogFaultIfPresent();
  // Returns true to keep running; false to exit the TickFrame loop.
  bool HandlePostStepState();
  void DetachMicroOpRecorderForFreeRun();
  void RestoreMicroOpRecorderForPause();

  SNES& snes_;
  BreakpointSet& breakpoints_;
  TraceLog& trace_log_;
  ErrorLog& error_log_;
  RunState state_ = RunState::kPaused;
  PauseReason pause_reason_ = PauseReason::kUser;
  std::optional<SnesAddrT> logged_fault_pc_ = std::nullopt;
  MicroOpRecorder* saved_microop_recorder_ = nullptr;
};

}  // namespace pupsnes::debugger
