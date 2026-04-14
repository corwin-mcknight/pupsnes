#pragma once

#include "pupsnes/hw/scheduler.h"

#include <cstddef>

namespace pupsnes {

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

    static bool hasPendingRun(const Scheduler &scheduler, device_id_t device_id) {
        const auto *state = scheduler.findRunState(device_id);
        return state != nullptr && state->has_pending_run;
    }

    static time_master_t pendingRunTime(const Scheduler &scheduler, device_id_t device_id) {
        const auto *state = scheduler.findRunState(device_id);
        return (state == nullptr) ? 0 : state->pending_run_time;
    }

    static uint64_t runGeneration(const Scheduler &scheduler, device_id_t device_id) {
        const auto *state = scheduler.findRunState(device_id);
        return (state == nullptr) ? 0 : state->run_generation;
    }

    static token_id_t blockedToken(const Scheduler &scheduler, device_id_t device_id) {
        const auto *state = scheduler.findRunState(device_id);
        return (state == nullptr) ? 0 : state->blocked_token;
    }

    static std::size_t queuedRunEventsFor(const Scheduler &scheduler, device_id_t device_id) {
        Scheduler::EventMinHeap queue = scheduler.eventQueue;
        std::size_t count = 0;

        while (!queue.empty()) {
            const SchedulerEvent &event = queue.top();
            if (event.subphase == SchedulerPhase::Run && event.type == EventType::DeviceRun && event.source != nullptr &&
                event.source->getDeviceId() == device_id) {
                count++;
            }
            queue.pop();
        }

        return count;
    }
};

} // namespace pupsnes
