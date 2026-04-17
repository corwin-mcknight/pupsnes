#pragma once

#include <cstdint>
#include <queue>
#include <tuple>
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
    return std::tie(a.time, a.subphase, a.type, a.seq) > std::tie(b.time, b.subphase, b.type, b.seq);
  }
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

  [[nodiscard]] DeviceRunState& EnsureRunState(DeviceIdT device_id);
  [[nodiscard]] const DeviceRunState* FindRunState(DeviceIdT device_id) const;
  void AlignDeviceTime(Device* device, TimeMasterT time);
  void ClearPendingRun(DeviceRunState& state);
  void ResetZeroProgressGuard(DeviceRunState& state);
  void RecordZeroProgressRun(DeviceRunState& state, const Device& device);
  void ValidateTickResult(const Device& device, const TickResult& result, TimeMasterDeltaT budget) const;
  [[nodiscard]] bool IsStaleRunEvent(const SchedulerEvent& event) const;
  void DiscardStaleRunEventsAtHead();
  void HandleRunResult(Device* device, const TickResult& result);

  friend struct SchedulerTestAccess;

 public:
  constexpr static TimeMasterT kMaxCyclesStep = 10;
  constexpr static TimeMasterT kMaxSameStepIterations = 10;

  explicit Scheduler(SNES* snes);
  ~Scheduler();

  void ScheduleEvent(TimeMasterT time, Device* source, SchedulerPhase subphase, EventType type,
                     uint64_t run_generation = 0);
  void ScheduleDeviceRun(Device* device, TimeMasterT time);

  void Step();
  [[nodiscard]] TimeMasterDeltaT ComputeBudget(TimeMasterT now) const;

  TokenIdT CreateToken(const TokenCreateParams& params);
  [[nodiscard]] const Token* GetToken(TokenIdT id) const;
  void RemoveToken(TokenIdT id);

  void CatchUpDevice(DeviceIdT device_id, TimeMasterT target_time);

  void DebugPrintNextEvent();
  void DebugPrintEventQueue();
};

}  // namespace pupsnes
