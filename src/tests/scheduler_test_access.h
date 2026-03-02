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
};

} // namespace pupsnes
