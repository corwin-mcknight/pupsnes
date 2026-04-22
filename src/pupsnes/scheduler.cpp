#include "pupsnes/hw/scheduler.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "pupsnes/hw/device.h"
#include "pupsnes/hw/snes.h"

namespace pupsnes {

namespace {
[[noreturn]] void FailScheduler(const char* message) { throw std::logic_error(message); }
}  // namespace

namespace {
constexpr std::size_t kReservedEventCapacity = 64;
constexpr std::size_t kReservedDeviceCount = 16;
}  // namespace

Scheduler::Scheduler(SNES* snes) : snes_(snes) {
  std::vector<SchedulerEvent> init;
  init.reserve(kReservedEventCapacity);
  eventQueue_ = EventMinHeap(SchedulerEventComparator{}, std::move(init));
  device_run_states_.reserve(kReservedDeviceCount);
}
Scheduler::~Scheduler() = default;

void Scheduler::Reset() {
  phase_ = SchedulerPhase::kCommitComplete;
  std::vector<SchedulerEvent> init;
  init.reserve(kReservedEventCapacity);
  eventQueue_ = EventMinHeap(SchedulerEventComparator{}, std::move(init));
  device_run_states_.clear();
  nextEventSeq_ = 0;
  token_table_ = TokenTable{};
}

bool Scheduler::HasPendingRunAtOrBefore(DeviceIdT device_id, TimeMasterT time) const {
  const DeviceRunState* state = FindRunState(device_id);
  return state != nullptr && state->has_pending_run && state->pending_run_time <= time;
}

void Scheduler::AlignDeviceTime(Device* device, TimeMasterT time) {
  if (device == nullptr) {
    return;
  }
  if (device->GetTime() > time) {
    FailScheduler("Scheduler attempted to run a device in the past");
  }
  if (device->GetTime() < time) {
    device->AdvanceLocalTime(time - device->GetTime());
  }
}

void Scheduler::RecordZeroProgressRun(DeviceRunState& state, const Device& device) {
  if (state.zero_progress_count == 0 || state.zero_progress_time != device.GetTime()) {
    state.zero_progress_time = device.GetTime();
    state.zero_progress_count = 1;
    return;
  }

  FailScheduler("Scheduler detected repeated same-time zero-progress Run dispatch");
}

void Scheduler::ValidateTickResult(const Device& device, const TickResult& result, TimeMasterDeltaT budget) const {
  // Strict budget enforcement: devices never exceed their granted cycles.
  // The CPU's Tick refuses to start any micro-op whose worst-case cost
  // wouldn't fit; the PPU's dot loop tracks partial-dot progress and stops
  // exactly on budget. A breach here is a device contract violation.
  if (result.completed_cycles > budget) {
    FailScheduler("Device tick exceeded scheduler budget");
  }

  if (result.reason == TickStopReason::kContinue) {
    FailScheduler("kContinue is an internal sentinel and must not escape Tick()");
  }

  // blocked_token is legal only for kBlockedOnToken, and required there.
  const bool is_blocked = result.reason == TickStopReason::kBlockedOnToken;
  if (is_blocked == (result.blocked_token == 0)) {
    FailScheduler(is_blocked ? "kBlockedOnToken requires a token"
                             : "Only kBlockedOnToken may carry a blocked token");
  }

  // next_wake_time is legal only for kReachedLocalBoundary / kNoWork,
  // and required for kReachedLocalBoundary.
  const bool wake_allowed = result.reason == TickStopReason::kReachedLocalBoundary ||
                            result.reason == TickStopReason::kNoWork;
  if (!wake_allowed && result.HasWakeTime()) {
    FailScheduler("Only kReachedLocalBoundary / kNoWork may carry a wake time");
  }
  if (result.reason == TickStopReason::kReachedLocalBoundary && !result.HasWakeTime()) {
    FailScheduler("kReachedLocalBoundary requires next_wake_time");
  }

  const TimeMasterT committed_time = device.GetTime() + result.completed_cycles;
  if (result.HasWakeTime() && result.next_wake_time < committed_time) {
    FailScheduler("Scheduler received a wake time earlier than committed device time");
  }
}

bool Scheduler::IsStaleRunEvent(const SchedulerEvent& event) const {
  if (event.subphase != SchedulerPhase::kRun || event.type != EventType::kDeviceRun || event.source == nullptr) {
    return false;
  }

  const DeviceRunState* state = FindRunState(event.source->GetDeviceId());
  if (state == nullptr) {
    return true;
  }

  return !state->has_pending_run || event.run_generation != state->run_generation ||
         event.time != state->pending_run_time;
}

void Scheduler::DiscardStaleRunEventsAtHead() {
  while (!eventQueue_.empty() && IsStaleRunEvent(eventQueue_.top())) {
    eventQueue_.pop();
  }
}

void Scheduler::HandleRunResult(Device* device, const TickResult& result) {
  if (device == nullptr) {
    return;
  }

  DeviceRunState& state = EnsureRunState(device->GetDeviceId());

  switch (result.reason) {
    case TickStopReason::kBlockedOnToken:
      ClearPendingRun(state);
      ResetZeroProgressGuard(state);
      state.blocked_token = result.blocked_token;
      token_table_.SetBlocked(result.blocked_token, device->GetDeviceId());
      return;
    case TickStopReason::kNoWork:
      if (!result.HasWakeTime()) {
        ClearPendingRun(state);
        ResetZeroProgressGuard(state);
        return;
      }
      break;
    case TickStopReason::kFaulted:
    case TickStopReason::kDebuggerBreakpoint:
    case TickStopReason::kDebuggerStepComplete:
      ClearPendingRun(state);
      ResetZeroProgressGuard(state);
      return;
    case TickStopReason::kBudgetExhausted:
    case TickStopReason::kReachedLocalBoundary: break;
    case TickStopReason::kContinue:
      FailScheduler("kContinue is an internal sentinel and must not escape Tick()");
      return;
  }

  TimeMasterT next_run_time = device->GetTime();
  if ((result.reason == TickStopReason::kReachedLocalBoundary || result.reason == TickStopReason::kNoWork) &&
      result.HasWakeTime()) {
    next_run_time = result.next_wake_time;
  }

  const bool zero_progress_same_time = result.completed_cycles == 0 && next_run_time == device->GetTime();
  if (zero_progress_same_time) {
    RecordZeroProgressRun(state, *device);
  } else {
    ResetZeroProgressGuard(state);
  }

  ScheduleDeviceRun(device, next_run_time);
}

void Scheduler::ScheduleDeviceRun(Device* device, TimeMasterT time) {
  if (device == nullptr) {
    FailScheduler("Cannot schedule a null device run");
  }
  if (time < device->GetTime()) {
    FailScheduler("Cannot schedule a device run before committed device time");
  }

  DeviceRunState& state = EnsureRunState(device->GetDeviceId());
  state.run_generation++;
  state.has_pending_run = true;
  state.pending_run_time = time;
  state.blocked_token = 0;

  ScheduleEvent(time, device, SchedulerPhase::kRun, EventType::kDeviceRun, state.run_generation);
}

void Scheduler::Step() {
  if (snes_ == nullptr) {
    return;
  }

  DiscardStaleRunEventsAtHead();
  if (eventQueue_.empty()) {
    return;
  }

  SchedulerEvent event = eventQueue_.top();
  eventQueue_.pop();

  if (event.time < snes_->GetMasterTime()) {
    FailScheduler("Scheduler event in the past");
  }

  phase_ = event.subphase;
  snes_->SetMasterTime(event.time);

  switch (event.subphase) {
    case SchedulerPhase::kCommitComplete: {
      auto woken = token_table_.ResolveAt(event.time);
      for (const auto& wake : woken) {
        Device* device = snes_->GetDevice(wake.device_id);
        if (device == nullptr) {
          continue;
        }

        DeviceRunState& state = EnsureRunState(wake.device_id);
        if (state.blocked_token == wake.token_id) {
          ScheduleDeviceRun(device, event.time);
        }
      }

      if (event.source != nullptr) {
        event.source->OnEvent(event);
      }
      break;
    }
    case SchedulerPhase::kWakeSample:
      if (event.source != nullptr) {
        event.source->OnEvent(event);
      }
      break;
    case SchedulerPhase::kRun: {
      if (event.type != EventType::kDeviceRun) {
        if (event.source != nullptr) {
          event.source->OnEvent(event);
        }
        break;
      }

      if (event.source == nullptr || IsStaleRunEvent(event)) {
        return;
      }

      DeviceRunState& state = EnsureRunState(event.source->GetDeviceId());
      state.has_pending_run = false;
      state.pending_run_time = 0;
      state.blocked_token = 0;

      AlignDeviceTime(event.source, event.time);
      DiscardStaleRunEventsAtHead();

      const TimeMasterDeltaT budget = ComputeBudget(event.time);
      auto result = event.source->Tick(budget);
      ValidateTickResult(*event.source, result, budget);
      event.source->AdvanceLocalTime(result.completed_cycles);
      // Master time is the simulated-wall-clock front, not just the dispatch
      // pointer. The device has committed `completed_cycles` of work; advance
      // master to match so later readers (debug UI, error logs, fault
      // timestamps) see the true simulated present. The budget was clipped to
      // the next pre-existing event, and the only mid-tick event source
      // (FollowScheduled → CreateToken) pairs creation with immediate device
      // block at the cycle of creation — both guarantee no queued event lands
      // in `[event.time, event.time + completed_cycles)`.
      snes_->SetMasterTime(event.time + result.completed_cycles);
      HandleRunResult(event.source, result);
      break;
    }
  }
}

TimeMasterDeltaT Scheduler::ComputeBudget(TimeMasterT now) const {
  if (!eventQueue_.empty() && !IsStaleRunEvent(eventQueue_.top()) && eventQueue_.top().time >= now) {
    return std::min(kMaxCyclesStep, eventQueue_.top().time - now);
  }
  return kMaxCyclesStep;
}

TokenIdT Scheduler::CreateToken(const TokenCreateParams& params) {
  TokenIdT id = token_table_.Create(params);
  Device* device = snes_->GetDevice(params.source_device);
  ScheduleEvent(params.completion_time, device, SchedulerPhase::kCommitComplete, EventType::kDeviceRun);
  return id;
}

const Token* Scheduler::GetToken(TokenIdT id) const { return token_table_.Get(id); }

void Scheduler::RemoveToken(TokenIdT id) { token_table_.Remove(id); }

void Scheduler::CatchUpDevice(DeviceIdT device_id, TimeMasterT target_time) {
  Device* device = snes_->GetDevice(device_id);
  if (device == nullptr) {
    return;
  }
  if (device->GetTime() >= target_time) {
    return;
  }

  DeviceRunState& state = EnsureRunState(device_id);
  if (state.blocked_token != 0) {
    FailScheduler("Same-clock catch-up cannot run a device blocked on a token");
  }

  ClearPendingRun(state);
  ResetZeroProgressGuard(state);

  while (device->GetTime() < target_time) {
    const TimeMasterDeltaT budget = target_time - device->GetTime();
    auto result = device->Tick(budget);
    ValidateTickResult(*device, result, budget);

    if (result.completed_cycles == 0) {
      FailScheduler("Same-clock catch-up made no progress");
    }

    device->AdvanceLocalTime(result.completed_cycles);
    const bool reached_target = device->GetTime() >= target_time;

    switch (result.reason) {
      case TickStopReason::kBudgetExhausted:
        if (reached_target) {
          HandleRunResult(device, result);
        }
        break;
      case TickStopReason::kReachedLocalBoundary:
        if (!result.HasWakeTime()) {
          FailScheduler("ReachedLocalBoundary requires next_wake_time during catch-up");
        }
        if (reached_target) {
          HandleRunResult(device, result);
          break;
        }
        if (result.next_wake_time > target_time) {
          FailScheduler(
              "Same-clock catch-up hit a local boundary beyond the requested "
              "target");
        }
        AlignDeviceTime(device, result.next_wake_time);
        break;
      case TickStopReason::kBlockedOnToken: FailScheduler("Same-clock catch-up cannot block on a token"); break;
      case TickStopReason::kNoWork:
        if (!reached_target) {
          FailScheduler(
              "Same-clock catch-up cannot stop with NoWork before the target "
              "time");
        }
        HandleRunResult(device, result);
        break;
      case TickStopReason::kFaulted:
      case TickStopReason::kDebuggerBreakpoint:
      case TickStopReason::kDebuggerStepComplete: HandleRunResult(device, result); return;
      case TickStopReason::kContinue:
        FailScheduler("kContinue is an internal sentinel and must not escape Tick()");
        return;
    }
  }
}

std::vector<SchedulerEventView> Scheduler::SnapshotQueue() const {
  EventMinHeap temp_queue = eventQueue_;
  std::vector<SchedulerEventView> snapshot;
  snapshot.reserve(temp_queue.size());

  while (!temp_queue.empty()) {
    const SchedulerEvent& event = temp_queue.top();
    temp_queue.pop();

    if (IsStaleRunEvent(event)) {
      continue;
    }

    snapshot.push_back({
        .time = event.time,
        .device_id = (event.source != nullptr) ? std::optional<DeviceIdT>(event.source->GetDeviceId()) : std::nullopt,
        .subphase = event.subphase,
        .type = event.type,
        .run_generation = event.run_generation,
    });
  }

  return snapshot;
}

void Scheduler::DebugPrintEventQueue() {
  const std::vector<SchedulerEventView> snapshot = SnapshotQueue();
  if (snapshot.empty()) {
    std::cerr << "Event queue is empty.\n";
    return;
  }

  std::cerr << "Scheduled Events:\n";
  for (const SchedulerEventView& event : snapshot) {
    std::cerr << std::format("  Time: {}, Source: {}, Subphase: {}, Type: {}, Run generation: {}\n", event.time,
                             event.device_id.has_value() ? std::to_string(*event.device_id) : std::string("null"),
                             static_cast<int>(event.subphase), static_cast<int>(event.type), event.run_generation);
  }
}

}  // namespace pupsnes
