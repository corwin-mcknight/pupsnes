#include "pupsnes/hw/scheduler.h"

#include "pupsnes/hw/device.h"
#include "pupsnes/hw/snes.h"

#include <algorithm>
#include <cstdio>

namespace pupsnes {

namespace {
void debugPrintSchedulerEvent(const SchedulerEvent &event, const char *indent, bool multiline) {
    if (multiline) {
        printf("%sTime: %llu\n", indent, static_cast<unsigned long long>(event.time));
        printf("%sSource: %p\n", indent, static_cast<void *>(event.source));
        printf("%sSeq: %llu\n", indent, static_cast<unsigned long long>(event.seq));
        printf("%sSubphase: %d\n", indent, static_cast<int>(event.subphase));
        printf("%sType: %d\n", indent, static_cast<int>(event.type));
        return;
    }

    printf("%sTime: %llu, Source: %p, Seq: %llu, Subphase: %d, Type: %d\n", indent,
           static_cast<unsigned long long>(event.time), static_cast<void *>(event.source),
           static_cast<unsigned long long>(event.seq), static_cast<int>(event.subphase), static_cast<int>(event.type));
}
} // namespace

Scheduler::Scheduler(SNES *snes) : snes(snes) {}
Scheduler::~Scheduler() = default;

void Scheduler::scheduleEvent(time_master_t time, Device *source, SchedulerPhase subphase, EventType type) {
    SchedulerEvent event(time, source, nextEventSeq++, subphase, type);
    eventQueue.push(event);
}

void Scheduler::step() {
    if (eventQueue.empty() || snes == nullptr) {
        return;
    }

    // Obtain the next event
    SchedulerEvent event = eventQueue.top();
    eventQueue.pop();

    // Is this event valid (scheduled for now or in the future?) we don't support past events.
    if (event.time < snes->getMasterTime()) {
        printf("Warning: Scheduler event in the past. Event time: %llu, Current time: %llu\n",
               static_cast<unsigned long long>(event.time), static_cast<unsigned long long>(snes->getMasterTime()));
        return;
    }

    // Advance global time to this event's time.
    snes->setMasterTime(event.time);

    if (event.source == nullptr) {
        return;
    }

    switch (event.subphase) {
    case SchedulerPhase::CommitComplete:
    case SchedulerPhase::WakeSample:
        event.source->onEvent(event);
        break;
    case SchedulerPhase::Run: {
        if (event.type == EventType::DeviceRun) {
            event.source->tick(computeBudget(event.time));
        } else {
            event.source->onEvent(event);
        }
        break;
    }
    }
}

time_master_delta_t Scheduler::computeBudget(time_master_t now) const {
    time_master_delta_t budget = MAX_CYCLES_STEP;
    if (!eventQueue.empty()) {
        time_master_t next_time = eventQueue.top().time;
        if (next_time > now) {
            budget = std::min(budget, next_time - now);
        }
    }
    return budget;
}

void Scheduler::debugPrintNextEvent() {
    if (eventQueue.empty()) {
        printf("No scheduled events.\n");
        return;
    }
    const SchedulerEvent &event = eventQueue.top();
    printf("Next Event:\n");
    debugPrintSchedulerEvent(event, "  ", true);
}

void Scheduler::debugPrintEventQueue() {
    if (eventQueue.empty()) {
        printf("Event queue is empty.\n");
        return;
    }

    // Create a copy of the event queue to print without modifying the original
    EventMinHeap tempQueue = eventQueue;
    printf("Scheduled Events:\n");
    while (!tempQueue.empty()) {
        const SchedulerEvent &event = tempQueue.top();
        debugPrintSchedulerEvent(event, "  ", false);
        tempQueue.pop();
    }
}

} // namespace pupsnes
