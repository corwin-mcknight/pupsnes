#pragma once

#include <cstdint>
#include <optional>
#include <queue>
#include <vector>

#include "pupsnes/hw/token.h"
#include "pupsnes/types.h"

namespace pupsnes {

class SNES;
class Device;
struct TickResult;

/// Phases a scheduler may be in.
enum class SchedulerPhase : uint8_t { kCommitComplete = 0, kWakeSample = 1, kRun = 2 };

/// Priority levels for scheduled events.
enum class EventType : uint8_t {
  kDeviceRun = 0,       // Normal device execution, scheduled rarely but is how to
                        // "unpause" a device.
  kDeviceBoundary = 1,  // Device running normally but is communicating a sync boundary.
};

/// An event scheduled in the scheduler.
struct SchedulerEvent {
  TimeMasterT time;
  Device* source;  // Non-owning. Caller-owned device that generated this event.
  EventSeqT seq;
  SchedulerPhase subphase;
  EventType type;
  uint64_t run_generation = 0;
};

struct SchedulerEventComparator {
  bool operator()(const SchedulerEvent& a, const SchedulerEvent& b) const {
    if (a.time != b.time) return a.time > b.time;
    if (a.subphase != b.subphase) return a.subphase > b.subphase;
    if (a.type != b.type) return a.type > b.type;
    return a.seq > b.seq;
  }
};

struct SchedulerEventView {
  TimeMasterT time = 0;
  std::optional<DeviceIdT> device_id = std::nullopt;
  SchedulerPhase subphase = SchedulerPhase::kCommitComplete;
  EventType type = EventType::kDeviceRun;
  uint64_t run_generation = 0;
};

class Scheduler {
 private:
  using EventMinHeap = std::priority_queue<SchedulerEvent, std::vector<SchedulerEvent>, SchedulerEventComparator>;
  struct DeviceRunState {
    bool has_pending_run = false;
    TimeMasterT pending_run_time = 0;
    uint64_t run_generation = 0;
    TokenIdT blocked_token = 0;
    TimeMasterT zero_progress_time = 0;
    uint32_t zero_progress_count = 0;
  };

  SNES* snes_;  // Non-owning. SNES owns this Scheduler; pointer back to parent.
  SchedulerPhase phase_ = SchedulerPhase::kCommitComplete;
  EventMinHeap eventQueue_;
  std::vector<DeviceRunState> device_run_states_;

  EventSeqT nextEventSeq_ = 0;
  TokenTable token_table_;

  DeviceRunState& EnsureRunState(DeviceIdT device_id) {
    if (device_id >= device_run_states_.size()) {
      device_run_states_.resize(static_cast<std::size_t>(device_id) + 1U);
    }
    return device_run_states_[device_id];
  }
  [[nodiscard]] const DeviceRunState* FindRunState(DeviceIdT device_id) const {
    if (device_id >= device_run_states_.size()) {
      return nullptr;
    }
    return &device_run_states_[device_id];
  }
  static void ClearPendingRun(DeviceRunState& state) {
    state.has_pending_run = false;
    state.pending_run_time = 0;
    state.blocked_token = 0;
  }
  static void ResetZeroProgressGuard(DeviceRunState& state) {
    state.zero_progress_time = 0;
    state.zero_progress_count = 0;
  }
  void AlignDeviceTime(Device* device, TimeMasterT time);
  void RecordZeroProgressRun(DeviceRunState& state, const Device& device);
  void ValidateTickResult(const Device& device, const TickResult& result, TimeMasterDeltaT budget) const;
  [[nodiscard]] bool IsStaleRunEvent(const SchedulerEvent& event) const;
  void DiscardStaleRunEventsAtHead();
  void HandleRunResult(Device* device, const TickResult& result);

  friend struct SchedulerTestAccess;

 public:
  constexpr static TimeMasterT kMaxCyclesStep = 4096;
  constexpr static TimeMasterT kMaxSameStepIterations = 4096;

  explicit Scheduler(SNES* snes);
  ~Scheduler();

  void ScheduleEvent(TimeMasterT time, Device* source, SchedulerPhase subphase, EventType type,
                     uint64_t run_generation = 0) {
    eventQueue_.push({time, source, nextEventSeq_++, subphase, type, run_generation});
  }
  void ScheduleDeviceRun(Device* device, TimeMasterT time);

  void Step();
  [[nodiscard]] TimeMasterDeltaT ComputeBudget(TimeMasterT now) const;

  TokenIdT CreateToken(const TokenCreateParams& params);
  [[nodiscard]] const Token* GetToken(TokenIdT id) const;
  void RemoveToken(TokenIdT id);

  void CatchUpDevice(DeviceIdT device_id, TimeMasterT target_time);
  void Reset();
  [[nodiscard]] bool HasPendingEvents() const { return !eventQueue_.empty(); }
  [[nodiscard]] bool HasPendingRunAtOrBefore(DeviceIdT device_id, TimeMasterT time) const;
  [[nodiscard]] std::vector<SchedulerEventView> SnapshotQueue() const;

  void DebugPrintEventQueue();
};

}  // namespace pupsnes
