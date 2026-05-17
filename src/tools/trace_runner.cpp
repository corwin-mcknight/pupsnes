#include "pupsnes/tools/trace_runner.h"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "pupsnes/debugger/file_trace_sink.h"
#include "pupsnes/debugger/sha1.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/core/scheduler.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/hw/rom/rom_format.h"

namespace pupsnes::tools {

namespace {

// Validate the budget: exactly one of {instructions, master_cycles, frames}
// must be set. Returns a non-empty error string on misconfiguration.
[[nodiscard]] std::string ValidateBudget(const TraceStopBudget& b) {
  const int set_count =
      static_cast<int>(b.instructions.has_value()) +
      static_cast<int>(b.master_cycles.has_value()) +
      static_cast<int>(b.frames.has_value());
  if (set_count == 0) return "no stop budget specified";
  if (set_count > 1) return "more than one stop budget specified";
  return {};
}

[[nodiscard]] std::vector<uint8_t> ReadAllBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("cannot open ROM: " + path.string());
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] debugger::Sha1Digest ComputeSha1(const std::vector<uint8_t>& bytes) {
  debugger::Sha1 h;
  h.Update(bytes.data(), bytes.size());
  return h.Finalize();
}

// Returns true once the run loop should stop based on the configured budget,
// the current emitted-line count, and the master-time delta since boot.
[[nodiscard]] bool BudgetSatisfied(const TraceStopBudget& budget,
                                   uint64_t emitted_lines,
                                   TimeMasterT master_elapsed) {
  if (budget.instructions.has_value()) {
    return emitted_lines >= *budget.instructions;
  }
  if (budget.master_cycles.has_value()) {
    return master_elapsed >= *budget.master_cycles;
  }
  if (budget.frames.has_value()) {
    return master_elapsed >= static_cast<TimeMasterT>(*budget.frames) * kMasterCyclesPerFrame;
  }
  return true;  // unreachable — ValidateBudget rejects empty budgets
}

}  // namespace

TraceRunResult RunTrace(const TraceRunOptions& opts) {
  TraceRunResult result{};

  if (auto err = ValidateBudget(opts.budget); !err.empty()) {
    result.error = std::move(err);
    return result;
  }

  std::vector<uint8_t> rom_bytes;
  try {
    rom_bytes = ReadAllBytes(opts.rom_path);
  } catch (const std::exception& ex) {
    result.error = ex.what();
    return result;
  }
  StripSmcCopierHeader(rom_bytes);

  const debugger::Sha1Digest rom_sha1 = ComputeSha1(rom_bytes);

  SNES snes;
  const BuildResult load_result = snes.LoadRom(rom_bytes);
  if (!load_result.ok) {
    result.error = "ROM load failed: " + load_result.message;
    return result;
  }
  snes.Reset();

  debugger::FileTraceSink sink(snes, opts.output_path.string(), rom_sha1);
  if (sink.HasError()) {
    result.error = sink.Error();
    return result;
  }
  auto& contract = snes.GetCpu().MutableDebuggerContract();
  contract.trace_sink = &sink;
  if (opts.budget.instructions.has_value()) {
    // Lets the CPU yield with kRetiredStepTarget exactly after N retires
    // instead of overshooting on a multi-instruction TickToTarget call.
    contract.step_target = *opts.budget.instructions;
  }

  const TimeMasterT start_master = snes.GetMasterTime();
  // Defensive cap: never run more than this many TickToTarget loops without
  // making forward progress on master time. Prevents infinite loops if the
  // CPU stalls (e.g., STP) while the budget is in instruction-count mode.
  constexpr int kMaxStuckIterations = 16;
  int stuck_iterations = 0;
  TimeMasterT last_master = start_master;

  while (true) {
    const TimeMasterT now_master = snes.GetMasterTime();
    if (BudgetSatisfied(opts.budget, sink.LineCount(), now_master - start_master)) {
      break;
    }

    TimeMasterT next_event = snes.GetScheduler().NextEventMasterTime();
    // Clamp to the configured budget so we don't overshoot.
    TimeMasterT target = next_event;
    if (opts.budget.master_cycles.has_value()) {
      const TimeMasterT cap = start_master + *opts.budget.master_cycles;
      if (cap < target) target = cap;
    } else if (opts.budget.frames.has_value()) {
      const TimeMasterT cap =
          start_master + static_cast<TimeMasterT>(*opts.budget.frames) * kMasterCyclesPerFrame;
      if (cap < target) target = cap;
    }

    try {
      static_cast<void>(snes.GetCpu().TickToTarget(target));
    } catch (const std::exception& ex) {
      result.error = std::string{"CPU exception: "} + ex.what();
      sink.Flush();
      return result;
    }
    const TimeMasterT after_tick = snes.GetMasterTime();
    snes.MachineSync(after_tick);
    snes.GetScheduler().FireEventsThrough(after_tick);

    if (after_tick == last_master) {
      if (++stuck_iterations >= kMaxStuckIterations) {
        result.error = "CPU made no forward progress (STP/halt or tick budget too small)";
        sink.Flush();
        return result;
      }
    } else {
      stuck_iterations = 0;
      last_master = after_tick;
    }
  }

  sink.Flush();
  if (sink.HasError()) {
    result.error = sink.Error();
    return result;
  }

  result.ok = true;
  result.instructions_emitted = sink.LineCount();
  result.master_time_elapsed = snes.GetMasterTime() - start_master;
  return result;
}

}  // namespace pupsnes::tools
