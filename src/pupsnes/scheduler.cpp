#include "pupsnes/hw/scheduler.h"

#include "pupsnes/hw/device.h"
#include "pupsnes/hw/snes.h"

#include <algorithm>
#include <cassert>
#include <spdlog/spdlog.h>

namespace pupsnes {

namespace {
void debugPrintSchedulerEvent(const SchedulerEvent &event, bool multiline) {
    if (multiline) {
        spdlog::debug("  Time: {}", event.time);
        spdlog::debug("  Source: {}", static_cast<const void *>(event.source));
        spdlog::debug("  Seq: {}", event.seq);
        spdlog::debug("  Subphase: {}", static_cast<int>(event.subphase));
        spdlog::debug("  Type: {}", static_cast<int>(event.type));
        return;
    }

    spdlog::debug("  Time: {}, Source: {}, Seq: {}, Subphase: {}, Type: {}", event.time,
                  static_cast<const void *>(event.source), event.seq, static_cast<int>(event.subphase),
                  static_cast<int>(event.type));
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

    // Past events indicate a scheduling bug. Assert in debug, skip in release.
    if (event.time < snes->getMasterTime()) {
        assert(false && "Scheduler event in the past");
        return;
    }

    // Advance global time to this event's time.
    snes->setMasterTime(event.time);

    if (event.source == nullptr) {
        return;
    }

    switch (event.subphase) {
    case SchedulerPhase::CommitComplete: {
        auto woken = token_table_.resolveAt(event.time);
        for (device_id_t dev_id : woken) {
            Device *dev = snes->getDevice(dev_id);
            if (dev != nullptr) {
                scheduleEvent(event.time, dev, SchedulerPhase::Run, EventType::DeviceRun);
            }
        }
        event.source->onEvent(event);
        break;
    }
    case SchedulerPhase::WakeSample:
        event.source->onEvent(event);
        break;
    case SchedulerPhase::Run: {
        if (event.type == EventType::DeviceRun) {
            auto result = event.source->tick(computeBudget(event.time));
            event.source->advanceLocalTime(result.completedCycles);
            if (result.reason == TickStopReason::BlockedOnToken) {
                token_table_.setBlocked(result.blocked_token, event.source->getDeviceId());
            }
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
        if (next_time >= now) {
            budget = std::min(budget, next_time - now);
        }
    }
    return budget;
}

token_id_t Scheduler::createToken(TokenType type, device_id_t source,
                                  time_master_t completion_time, snes_addr_t address, uint8_t data) {
    token_id_t id = token_table_.create(type, source, completion_time, address, data);
    Device *device = snes->getDevice(source);
    scheduleEvent(completion_time, device, SchedulerPhase::CommitComplete, EventType::DeviceRun);
    return id;
}

const Token *Scheduler::getToken(token_id_t id) const { return token_table_.get(id); }

void Scheduler::removeToken(token_id_t id) { token_table_.remove(id); }

void Scheduler::debugPrintNextEvent() {
    if (eventQueue.empty()) {
        spdlog::debug("No scheduled events.");
        return;
    }
    const SchedulerEvent &event = eventQueue.top();
    spdlog::debug("Next Event:");
    debugPrintSchedulerEvent(event, true);
}

void Scheduler::debugPrintEventQueue() {
    if (eventQueue.empty()) {
        spdlog::debug("Event queue is empty.");
        return;
    }

    EventMinHeap tempQueue = eventQueue;
    spdlog::debug("Scheduled Events:");
    while (!tempQueue.empty()) {
        const SchedulerEvent &event = tempQueue.top();
        debugPrintSchedulerEvent(event, false);
        tempQueue.pop();
    }
}

} // namespace pupsnes
