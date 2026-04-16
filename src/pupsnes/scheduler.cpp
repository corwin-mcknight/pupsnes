#include "pupsnes/hw/scheduler.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>

#include "pupsnes/hw/device.h"
#include "pupsnes/hw/snes.h"

namespace pupsnes {

namespace {
[[noreturn]] void failScheduler(const char* message) { throw std::logic_error(message); }

void debugPrintSchedulerEvent(const SchedulerEvent& event, bool multiline) {
    if (multiline) {
        spdlog::debug("  Time: {}", event.time);
        spdlog::debug("  Source: {}", static_cast<const void*>(event.source));
        spdlog::debug("  Seq: {}", event.seq);
        spdlog::debug("  Subphase: {}", static_cast<int>(event.subphase));
        spdlog::debug("  Type: {}", static_cast<int>(event.type));
        spdlog::debug("  Run generation: {}", event.run_generation);
        return;
    }

    spdlog::debug("  Time: {}, Source: {}, Seq: {}, Subphase: {}, Type: {}, Run generation: {}", event.time,
                  static_cast<const void*>(event.source), event.seq, static_cast<int>(event.subphase),
                  static_cast<int>(event.type), event.run_generation);
}
}  // namespace

Scheduler::Scheduler(SNES* snes) : snes(snes) {}
Scheduler::~Scheduler() = default;

Scheduler::DeviceRunState& Scheduler::ensureRunState(device_id_t device_id) {
    if (device_id >= device_run_states_.size()) {
        device_run_states_.resize(static_cast<std::size_t>(device_id) + 1U);
    }
    return device_run_states_[device_id];
}

const Scheduler::DeviceRunState* Scheduler::findRunState(device_id_t device_id) const {
    if (device_id >= device_run_states_.size()) {
        return nullptr;
    }
    return &device_run_states_[device_id];
}

void Scheduler::alignDeviceTime(Device* device, time_master_t time) {
    if (device == nullptr) {
        return;
    }
    if (device->getTime() > time) {
        failScheduler("Scheduler attempted to run a device in the past");
    }
    if (device->getTime() < time) {
        device->advanceLocalTime(time - device->getTime());
    }
}

void Scheduler::clearPendingRun(DeviceRunState& state) {
    state.has_pending_run = false;
    state.pending_run_time = 0;
    state.blocked_token = 0;
}

void Scheduler::resetZeroProgressGuard(DeviceRunState& state) {
    state.zero_progress_time = 0;
    state.zero_progress_count = 0;
}

void Scheduler::recordZeroProgressRun(DeviceRunState& state, const Device& device) {
    if (state.zero_progress_count == 0 || state.zero_progress_time != device.getTime()) {
        state.zero_progress_time = device.getTime();
        state.zero_progress_count = 1;
        return;
    }

    failScheduler("Scheduler detected repeated same-time zero-progress Run dispatch");
}

void Scheduler::validateTickResult(const Device& device, const TickResult& result, time_master_delta_t budget) const {
    if (result.completed_cycles > budget) {
        failScheduler("Device tick exceeded scheduler budget");
    }

    const time_master_t committed_time = device.getTime() + result.completed_cycles;

    switch (result.reason) {
        case TickStopReason::BudgetExhausted:
            if (result.blocked_token != 0) {
                failScheduler("BudgetExhausted cannot carry a blocked token");
            }
            if (result.next_wake_time.has_value()) {
                failScheduler("BudgetExhausted cannot carry a wake time");
            }
            break;
        case TickStopReason::ReachedLocalBoundary:
            if (result.blocked_token != 0) {
                failScheduler("ReachedLocalBoundary cannot carry a blocked token");
            }
            if (!result.next_wake_time.has_value()) {
                failScheduler("ReachedLocalBoundary requires next_wake_time");
            }
            break;
        case TickStopReason::BlockedOnToken:
            if (result.blocked_token == 0) {
                failScheduler("BlockedOnToken requires a token");
            }
            if (result.next_wake_time.has_value()) {
                failScheduler("BlockedOnToken cannot carry a wake time");
            }
            break;
        case TickStopReason::NoWork:
            if (result.blocked_token != 0) {
                failScheduler("NoWork cannot carry a blocked token");
            }
            break;
    }

    if (result.next_wake_time.has_value() && *result.next_wake_time < committed_time) {
        failScheduler("Scheduler received a wake time earlier than committed device time");
    }
}

bool Scheduler::isStaleRunEvent(const SchedulerEvent& event) const {
    if (event.subphase != SchedulerPhase::Run || event.type != EventType::DeviceRun || event.source == nullptr) {
        return false;
    }

    const DeviceRunState* state = findRunState(event.source->getDeviceId());
    if (state == nullptr) {
        return true;
    }

    return !state->has_pending_run || event.run_generation != state->run_generation ||
           event.time != state->pending_run_time;
}

void Scheduler::discardStaleRunEventsAtHead() {
    while (!eventQueue.empty() && isStaleRunEvent(eventQueue.top())) {
        eventQueue.pop();
    }
}

void Scheduler::handleRunResult(Device* device, const TickResult& result) {
    if (device == nullptr) {
        return;
    }

    DeviceRunState& state = ensureRunState(device->getDeviceId());

    switch (result.reason) {
        case TickStopReason::BlockedOnToken:
            clearPendingRun(state);
            resetZeroProgressGuard(state);
            state.blocked_token = result.blocked_token;
            token_table_.setBlocked(result.blocked_token, device->getDeviceId());
            return;
        case TickStopReason::NoWork:
            if (!result.next_wake_time.has_value()) {
                clearPendingRun(state);
                resetZeroProgressGuard(state);
                return;
            }
            break;
        case TickStopReason::BudgetExhausted:
        case TickStopReason::ReachedLocalBoundary:
            break;
    }

    time_master_t next_run_time = device->getTime();
    if (result.reason == TickStopReason::ReachedLocalBoundary || result.reason == TickStopReason::NoWork) {
        next_run_time = *result.next_wake_time;
    }

    const bool zero_progress_same_time = result.completed_cycles == 0 && next_run_time == device->getTime();
    if (zero_progress_same_time) {
        recordZeroProgressRun(state, *device);
    } else {
        resetZeroProgressGuard(state);
    }

    scheduleDeviceRun(device, next_run_time);
}

void Scheduler::scheduleEvent(time_master_t time, Device* source, SchedulerPhase subphase, EventType type,
                              uint64_t run_generation) {
    eventQueue.push({time, source, nextEventSeq++, subphase, type, run_generation});
}

void Scheduler::scheduleDeviceRun(Device* device, time_master_t time) {
    if (device == nullptr) {
        failScheduler("Cannot schedule a null device run");
    }
    if (time < device->getTime()) {
        failScheduler("Cannot schedule a device run before committed device time");
    }

    DeviceRunState& state = ensureRunState(device->getDeviceId());
    state.run_generation++;
    state.has_pending_run = true;
    state.pending_run_time = time;
    state.blocked_token = 0;

    scheduleEvent(time, device, SchedulerPhase::Run, EventType::DeviceRun, state.run_generation);
}

void Scheduler::step() {
    if (snes == nullptr) {
        return;
    }

    discardStaleRunEventsAtHead();
    if (eventQueue.empty()) {
        return;
    }

    SchedulerEvent event = eventQueue.top();
    eventQueue.pop();

    if (event.time < snes->getMasterTime()) {
        failScheduler("Scheduler event in the past");
    }

    phase = event.subphase;
    snes->setMasterTime(event.time);

    switch (event.subphase) {
        case SchedulerPhase::CommitComplete: {
            auto woken = token_table_.resolveAt(event.time);
            for (const auto& wake : woken) {
                Device* device = snes->getDevice(wake.device_id);
                if (device == nullptr) {
                    continue;
                }

                DeviceRunState& state = ensureRunState(wake.device_id);
                if (state.blocked_token == wake.token_id) {
                    scheduleDeviceRun(device, event.time);
                }
            }

            if (event.source != nullptr) {
                event.source->onEvent(event);
            }
            break;
        }
        case SchedulerPhase::WakeSample:
            if (event.source != nullptr) {
                event.source->onEvent(event);
            }
            break;
        case SchedulerPhase::Run: {
            if (event.type != EventType::DeviceRun) {
                if (event.source != nullptr) {
                    event.source->onEvent(event);
                }
                break;
            }

            if (event.source == nullptr || isStaleRunEvent(event)) {
                return;
            }

            DeviceRunState& state = ensureRunState(event.source->getDeviceId());
            state.has_pending_run = false;
            state.pending_run_time = 0;
            state.blocked_token = 0;

            alignDeviceTime(event.source, event.time);
            discardStaleRunEventsAtHead();

            const time_master_delta_t budget = computeBudget(event.time);
            auto result = event.source->tick(budget);
            validateTickResult(*event.source, result, budget);
            event.source->advanceLocalTime(result.completed_cycles);
            handleRunResult(event.source, result);
            break;
        }
    }
}

time_master_delta_t Scheduler::computeBudget(time_master_t now) const {
    if (!eventQueue.empty() && !isStaleRunEvent(eventQueue.top()) && eventQueue.top().time >= now) {
        return std::min(MAX_CYCLES_STEP, eventQueue.top().time - now);
    }
    return MAX_CYCLES_STEP;
}

token_id_t Scheduler::createToken(const TokenCreateParams& params) {
    token_id_t id = token_table_.create(params);
    Device* device = snes->getDevice(params.source_device);
    scheduleEvent(params.completion_time, device, SchedulerPhase::CommitComplete, EventType::DeviceRun);
    return id;
}

const Token* Scheduler::getToken(token_id_t id) const { return token_table_.get(id); }

void Scheduler::removeToken(token_id_t id) { token_table_.remove(id); }

void Scheduler::catchUpDevice(device_id_t device_id, time_master_t target_time) {
    Device* device = snes->getDevice(device_id);
    if (device == nullptr) {
        return;
    }
    if (device->getTime() >= target_time) {
        return;
    }

    DeviceRunState& state = ensureRunState(device_id);
    if (state.blocked_token != 0) {
        failScheduler("Same-clock catch-up cannot run a device blocked on a token");
    }

    clearPendingRun(state);
    resetZeroProgressGuard(state);

    while (device->getTime() < target_time) {
        const time_master_delta_t budget = target_time - device->getTime();
        auto result = device->tick(budget);
        validateTickResult(*device, result, budget);

        if (result.completed_cycles == 0) {
            failScheduler("Same-clock catch-up made no progress");
        }

        device->advanceLocalTime(result.completed_cycles);
        const bool reached_target = device->getTime() >= target_time;

        switch (result.reason) {
            case TickStopReason::BudgetExhausted:
                if (reached_target) {
                    handleRunResult(device, result);
                }
                break;
            case TickStopReason::ReachedLocalBoundary:
                if (!result.next_wake_time.has_value()) {
                    failScheduler("ReachedLocalBoundary requires next_wake_time during catch-up");
                }
                if (reached_target) {
                    handleRunResult(device, result);
                    break;
                }
                if (*result.next_wake_time > target_time) {
                    failScheduler("Same-clock catch-up hit a local boundary beyond the requested target");
                }
                alignDeviceTime(device, *result.next_wake_time);
                break;
            case TickStopReason::BlockedOnToken:
                failScheduler("Same-clock catch-up cannot block on a token");
                break;
            case TickStopReason::NoWork:
                if (!reached_target) {
                    failScheduler("Same-clock catch-up cannot stop with NoWork before the target time");
                }
                handleRunResult(device, result);
                break;
        }
    }
}

void Scheduler::debugPrintNextEvent() {
    if (eventQueue.empty()) {
        spdlog::debug("No scheduled events.");
        return;
    }
    const SchedulerEvent& event = eventQueue.top();
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
        const SchedulerEvent& event = tempQueue.top();
        debugPrintSchedulerEvent(event, false);
        tempQueue.pop();
    }
}

}  // namespace pupsnes
