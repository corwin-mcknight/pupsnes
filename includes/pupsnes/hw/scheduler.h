#pragma once

#include "pupsnes/types.h"

#include <cstddef>
#include <cstdint>
#include <queue>
#include <tuple>
#include <vector>

namespace pupsnes {

class SNES;
class Device;
struct SchedulerTestAccess;

/// Phases a scheduler may be in.
enum class SchedulerPhase : uint8_t { CommitComplete = 0, WakeSample = 1, Run = 2 };

/// Priority levels for scheduled events.
enum class EventType : uint8_t {
    DeviceRun = 0, // Normal device execution, scheduled rarely but is how to "unpause" a device.
    DeviceBoundary = 1, // Device running normally but is communicating a sync boundary.
};

/// An event scheduled in the scheduler.
struct SchedulerEvent {
    time_master_t time;
    Device *source;
    uint64_t seq;
    SchedulerPhase subphase;
    EventType type;

    SchedulerEvent(time_master_t time, Device *source, uint64_t seq, SchedulerPhase subphase,
                   EventType type)
        : time(time), source(source), seq(seq), subphase(subphase), type(type) {}
    SchedulerEvent()
        : time(0), source(nullptr), seq(0), subphase(SchedulerPhase::CommitComplete),
          type(EventType::DeviceRun) {}
};

struct SchedulerEventComparator {
    bool operator()(const SchedulerEvent &a, const SchedulerEvent &b) {
        return std::tie(a.time, a.subphase, a.type, a.seq) >
               std::tie(b.time, b.subphase, b.type, b.seq);
    }
};

class Scheduler {
  private:
    using EventMinHeap =
        std::priority_queue<SchedulerEvent, std::vector<SchedulerEvent>, SchedulerEventComparator>;

    SNES *snes;
    SchedulerPhase phase = SchedulerPhase::CommitComplete;
    EventMinHeap eventQueue;

    uint64_t nextEventSeq = 0;

    friend struct SchedulerTestAccess;

  public:
    constexpr static time_master_t MAX_CYCLES_STEP = 10;
    constexpr static time_master_t MAX_SAME_STEP_ITERATIONS = 10;

    Scheduler(SNES *snes);
    ~Scheduler();

    void scheduleEvent(time_master_t time, Device *source, SchedulerPhase subphase, EventType type);

    void step();
    time_master_delta_t computeBudget(time_master_t now) const;

    void debugPrintNextEvent();
    void debugPrintEventQueue();
};

#ifdef PUPSNES_TESTING
struct SchedulerTestAccess {
    static std::size_t eventQueueSize(const Scheduler &scheduler) {
        return scheduler.eventQueue.size();
    }
    static SchedulerEvent peekNextEvent(const Scheduler &scheduler) {
        return scheduler.eventQueue.top();
    }
    static SchedulerEvent popNextEvent(Scheduler &scheduler) {
        SchedulerEvent event = scheduler.eventQueue.top();
        scheduler.eventQueue.pop();
        return event;
    }
};
#endif
}; // namespace pupsnes
