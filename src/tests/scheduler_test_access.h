#pragma once

#include <cstddef>

#include "pupsnes/hw/scheduler.h"

namespace pupsnes {

struct SchedulerTestAccess {
    static std::size_t EventQueueSize(const Scheduler& scheduler) { return scheduler.eventQueue_.size(); }
    static SchedulerEvent PeekNextEvent(const Scheduler& scheduler) { return scheduler.eventQueue_.top(); }
    static SchedulerEvent PopNextEvent(Scheduler& scheduler) {
        SchedulerEvent event = scheduler.eventQueue_.top();
        scheduler.eventQueue_.pop();
        return event;
    }

    static bool HasPendingRun(const Scheduler& scheduler, DeviceIdT device_id) {
        const auto* state = scheduler.FindRunState(device_id);
        return state != nullptr && state->has_pending_run;
    }

    static TimeMasterT PendingRunTime(const Scheduler& scheduler, DeviceIdT device_id) {
        const auto* state = scheduler.FindRunState(device_id);
        return (state == nullptr) ? 0 : state->pending_run_time;
    }

    static uint64_t RunGeneration(const Scheduler& scheduler, DeviceIdT device_id) {
        const auto* state = scheduler.FindRunState(device_id);
        return (state == nullptr) ? 0 : state->run_generation;
    }

    static TokenIdT BlockedToken(const Scheduler& scheduler, DeviceIdT device_id) {
        const auto* state = scheduler.FindRunState(device_id);
        return (state == nullptr) ? 0 : state->blocked_token;
    }

    static std::size_t QueuedRunEventsFor(const Scheduler& scheduler, DeviceIdT device_id) {
        Scheduler::EventMinHeap queue = scheduler.eventQueue_;
        std::size_t count = 0;

        while (!queue.empty()) {
            const SchedulerEvent& event = queue.top();
            if (event.subphase == SchedulerPhase::kRun && event.type == EventType::kDeviceRun &&
                event.source != nullptr && event.source->GetDeviceId() == device_id) {
                count++;
            }
            queue.pop();
        }

        return count;
    }
};

}  // namespace pupsnes
