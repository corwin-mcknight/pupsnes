#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "pupsnes/core/types.h"

namespace pupsnes {
class SNES;
}

namespace pupsnes::tools {

// Drives the SNES master clock forward until master time reaches `cap`, using
// the canonical scheduler-bounded sequence (tick the CPU to min(next_event,
// cap), MachineSync, then fire due events). Loops with a stuck-guard that
// aborts if the CPU stops making forward progress (e.g. STP/halt or a tick
// budget too small). Returns std::nullopt on reaching the cap; a human-readable
// error string on a stall or an emulation exception.
//
// This is the single source of truth for the run-loop timing sequence shared by
// the trace runner and the screenshot tool — keep cycle-accurate ordering here.
[[nodiscard]] std::optional<std::string> DriveMachineToMasterTime(SNES& snes, TimeMasterT cap);

// Stop-condition for a trace run. Exactly one of these must be set in
// TraceRunOptions; the runner asserts when zero or more than one is supplied.
struct TraceStopBudget {
  std::optional<uint64_t> instructions;      // retire N instructions
  std::optional<TimeMasterT> master_cycles;  // run until master time has advanced by N
  std::optional<uint32_t> frames;            // run until N frames have completed
};

struct TraceRunOptions {
  std::filesystem::path rom_path;
  std::filesystem::path output_path;
  TraceStopBudget budget;
};

struct TraceRunResult {
  bool ok = false;
  std::string error;                  // populated when ok==false
  uint64_t instructions_emitted = 0;  // lines after the header
  TimeMasterT master_time_elapsed = 0;
};

// Loads `rom_path` (LoROM, with optional SMC copier header), boots a SNES,
// installs a FileTraceSink at `output_path`, and runs the machine until the
// stop budget is satisfied. Returns ok=true on a clean run; ok=false with a
// human-readable error otherwise.
TraceRunResult RunTrace(const TraceRunOptions& opts);

// Master cycles per nominal NTSC frame — used by --frames. Lives here so
// CLI argument parsing in the runner main can pre-compute targets without
// importing the FileTraceSink header.
inline constexpr TimeMasterT kMasterCyclesPerFrame = 262U * 1364U;

}  // namespace pupsnes::tools
