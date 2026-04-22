#pragma once

#include <cstdint>
#include <optional>

#include "pupsnes/hw/5a22/cpu_regs.h"
#include "pupsnes/types.h"

namespace pupsnes {

// Single trace record produced at instruction-begin: the master time, the PC
// that was just fetched, and the register state immediately before the opcode
// executed. Lives in the core namespace so CPU can produce it without layering
// into the debugger module.
struct TraceEntry {
  TimeMasterT master_time = 0;
  SnesAddrT pc = 0;
  CpuRegs regs{};
};

// Interface the CPU queries before each opcode fetch to decide whether to stop.
// AnyEnabled() lets callers short-circuit the per-instruction address lookup
// when no breakpoints are set — the common free-run case.
class BreakpointLookup {
 public:
  virtual ~BreakpointLookup() = default;
  [[nodiscard]] virtual bool AnyEnabled() const = 0;
  [[nodiscard]] virtual bool IsEnabled(SnesAddrT address) const = 0;
};

// Interface the CPU calls once per retired instruction to record a trace entry.
class TraceSink {
 public:
  virtual ~TraceSink() = default;
  virtual void Record(const TraceEntry& entry) = 0;
};

// Bundle of debugger-owned state the CPU needs to honor: which breakpoints are
// set, where trace records go, and how many more instructions to retire before
// stopping (0 = unbounded / free run).
//
// suppressed_breakpoint_pc lets a user resume from a hit breakpoint without
// immediately re-triggering: when set, the CPU skips the breakpoint check for
// that exact PC on the next fetch and clears the field.
struct DebuggerContract {
  enum class StepGranularity : uint8_t { kInstruction = 0, kMicroOp = 1 };

  const BreakpointLookup* breakpoints = nullptr;
  TraceSink* trace_sink = nullptr;
  uint64_t step_target = 0;
  std::optional<SnesAddrT> suppressed_breakpoint_pc = std::nullopt;
  StepGranularity step_granularity = StepGranularity::kInstruction;
};

}  // namespace pupsnes
