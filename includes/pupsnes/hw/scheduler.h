#pragma once

#include "pupsnes/hw/token.h"
#include "pupsnes/types.h"

#include <cstdint>
#include <queue>
#include <tuple>
#include <vector>

namespace pupsnes {

class SNES;
class Device;

/// Phases a scheduler may be in.
enum class SchedulerPhase : uint8_t { CommitComplete = 0, WakeSample = 1, Run = 2 };

/// Priority levels for scheduled events.
enum class EventType : uint8_t {
    DeviceRun = 0,      // Normal device execution, scheduled rarely but is how to "unpause" a device.
    DeviceBoundary = 1, // Device running normally but is communicating a sync boundary.
};

/// An event scheduled in the scheduler.
struct SchedulerEvent {
    time_master_t time;
    Device *source; // Non-owning. Caller-owned device that generated this event.
    event_seq_t seq;
    SchedulerPhase subphase;
    EventType type;
};

struct SchedulerEventComparator {
    bool operator()(const SchedulerEvent &a, const SchedulerEvent &b) const {
        return std::tie(a.time, a.subphase, a.type, a.seq) > std::tie(b.time, b.subphase, b.type, b.seq);
    }
};

class Scheduler {
  private:
    using EventMinHeap = std::priority_queue<SchedulerEvent, std::vector<SchedulerEvent>, SchedulerEventComparator>;

    SNES *snes; // Non-owning. SNES owns this Scheduler; pointer back to parent.
    SchedulerPhase phase = SchedulerPhase::CommitComplete;
    EventMinHeap eventQueue;

    event_seq_t nextEventSeq = 0;
    TokenTable token_table_;

    friend struct SchedulerTestAccess;

  public:
    constexpr static time_master_t MAX_CYCLES_STEP = 10;
    constexpr static time_master_t MAX_SAME_STEP_ITERATIONS = 10;

    Scheduler(SNES *snes);
    ~Scheduler();

    void scheduleEvent(time_master_t time, Device *source, SchedulerPhase subphase, EventType type);

    void step();
    [[nodiscard]] time_master_delta_t computeBudget(time_master_t now) const;

    token_id_t createToken(const TokenCreateParams &params);
    [[nodiscard]] const Token *getToken(token_id_t id) const;
    void removeToken(token_id_t id);

    void catchUpDevice(device_id_t device_id, time_master_t target_time);

    void debugPrintNextEvent();
    void debugPrintEventQueue();
};

} // namespace pupsnes
