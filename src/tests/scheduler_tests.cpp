#include <catch2/catch_test_macros.hpp>

#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/token.h"
#include "scheduler_test_access.h"

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

class ScriptedDevice : public pupsnes::Device {
  public:
    using TickScript = std::function<pupsnes::TickResult(ScriptedDevice &, pupsnes::time_master_delta_t)>;

    explicit ScriptedDevice(pupsnes::SNES *snes) : pupsnes::Device(snes) {}

    pupsnes::TickResult tick(pupsnes::time_master_delta_t budget) override {
        ++tick_calls;
        start_times.push_back(getTime());
        budgets.push_back(budget);

        if (scripts.empty()) {
            return {budget, pupsnes::TickStopReason::BudgetExhausted};
        }

        TickScript script = scripts.front();
        scripts.pop_front();
        return script(*this, budget);
    }

    void onEvent(const pupsnes::SchedulerEvent &event) override {
        ++event_calls;
        seen_events.push_back(event);
    }

    void pushScript(TickScript script) { scripts.push_back(std::move(script)); }

    void pushResult(const pupsnes::TickResult &result) {
        scripts.push_back([result](ScriptedDevice &, pupsnes::time_master_delta_t) { return result; });
    }

    int tick_calls = 0;
    int event_calls = 0;
    std::vector<pupsnes::time_master_t> start_times;
    std::vector<pupsnes::time_master_delta_t> budgets;
    std::vector<pupsnes::SchedulerEvent> seen_events;

  private:
    std::deque<TickScript> scripts;
};

class DynamicTokenObserverDevice : public pupsnes::Device {
  public:
    explicit DynamicTokenObserverDevice(pupsnes::SNES *snes) : pupsnes::Device(snes) {}

    pupsnes::TickResult tick(pupsnes::time_master_delta_t budget) override {
        ++tick_calls;
        last_budget = budget;
        if (watched_token != 0) {
            const auto *token = snes->scheduler->getToken(watched_token);
            saw_completed_token = token != nullptr && token->state == pupsnes::TokenState::Completed;
        }
        return {budget, pupsnes::TickStopReason::NoWork};
    }

    void onEvent(const pupsnes::SchedulerEvent &) override {}

    pupsnes::token_id_t watched_token = 0;
    bool saw_completed_token = false;
    int tick_calls = 0;
    pupsnes::time_master_delta_t last_budget = 0;
};

} // namespace

TEST_CASE("SNES assigns stable device IDs", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice dev1(&snes);
    ScriptedDevice dev2(&snes);
    ScriptedDevice dev3(&snes);

    REQUIRE(dev1.getDeviceId() == 0);
    REQUIRE(dev2.getDeviceId() == 1);
    REQUIRE(dev3.getDeviceId() == 2);

    REQUIRE(snes.getDevice(0) == &dev1);
    REQUIRE(snes.getDevice(1) == &dev2);
    REQUIRE(snes.getDevice(2) == &dev3);
    REQUIRE(snes.getDevice(99) == nullptr);
}

TEST_CASE("Scheduler orders events by time, subphase, type, and seq", "[unit]") {
    pupsnes::Scheduler scheduler(nullptr);
    ScriptedDevice device(nullptr);

    scheduler.scheduleEvent(5, &device, pupsnes::SchedulerPhase::Run, pupsnes::EventType::DeviceBoundary);
    scheduler.scheduleEvent(5, &device, pupsnes::SchedulerPhase::Run, pupsnes::EventType::DeviceRun);
    scheduler.scheduleEvent(5, &device, pupsnes::SchedulerPhase::CommitComplete, pupsnes::EventType::DeviceRun);
    scheduler.scheduleEvent(2, &device, pupsnes::SchedulerPhase::Run, pupsnes::EventType::DeviceRun);

    REQUIRE(pupsnes::SchedulerTestAccess::eventQueueSize(scheduler) == 4);

    auto first = pupsnes::SchedulerTestAccess::popNextEvent(scheduler);
    auto second = pupsnes::SchedulerTestAccess::popNextEvent(scheduler);
    auto third = pupsnes::SchedulerTestAccess::popNextEvent(scheduler);
    auto fourth = pupsnes::SchedulerTestAccess::popNextEvent(scheduler);

    REQUIRE(first.time == 2);
    REQUIRE(second.subphase == pupsnes::SchedulerPhase::CommitComplete);
    REQUIRE(third.type == pupsnes::EventType::DeviceRun);
    REQUIRE(fourth.type == pupsnes::EventType::DeviceBoundary);
}

TEST_CASE("Scheduler uses sequence as final tiebreaker", "[unit]") {
    pupsnes::Scheduler scheduler(nullptr);
    ScriptedDevice device(nullptr);

    scheduler.scheduleEvent(3, &device, pupsnes::SchedulerPhase::Run, pupsnes::EventType::DeviceRun);
    scheduler.scheduleEvent(3, &device, pupsnes::SchedulerPhase::Run, pupsnes::EventType::DeviceRun);

    auto first = pupsnes::SchedulerTestAccess::popNextEvent(scheduler);
    auto second = pupsnes::SchedulerTestAccess::popNextEvent(scheduler);

    REQUIRE(first.seq < second.seq);
}

TEST_CASE("Scheduler step advances time and dispatches non-run events", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice device(&snes);

    snes.scheduler->scheduleEvent(5, &device, pupsnes::SchedulerPhase::WakeSample, pupsnes::EventType::DeviceBoundary);

    snes.scheduler->step();

    REQUIRE(snes.getMasterTime() == 5);
    REQUIRE(device.event_calls == 1);
    REQUIRE(device.tick_calls == 0);
}

TEST_CASE("Scheduler aligns device time to its wake and commits completed cycles exactly once", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice device(&snes);

    device.pushResult({3, pupsnes::TickStopReason::NoWork});
    snes.scheduler->scheduleDeviceRun(&device, 10);
    snes.scheduler->step();

    REQUIRE(device.tick_calls == 1);
    REQUIRE(device.start_times.at(0) == 10);
    REQUIRE(device.getTime() == 13);
    REQUIRE_FALSE(pupsnes::SchedulerTestAccess::hasPendingRun(*snes.scheduler, device.getDeviceId()));
}

TEST_CASE("Scheduler reschedules BudgetExhausted devices at their committed time", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice device(&snes);

    device.pushResult({2, pupsnes::TickStopReason::BudgetExhausted});
    snes.scheduler->scheduleDeviceRun(&device, 10);
    snes.scheduler->step();

    REQUIRE(device.getTime() == 12);
    REQUIRE(pupsnes::SchedulerTestAccess::hasPendingRun(*snes.scheduler, device.getDeviceId()));
    REQUIRE(pupsnes::SchedulerTestAccess::pendingRunTime(*snes.scheduler, device.getDeviceId()) == 12);
}

TEST_CASE("Scheduler schedules local-boundary wakes authoritatively", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice device(&snes);

    device.pushResult({3, pupsnes::TickStopReason::ReachedLocalBoundary, 0, 20});
    snes.scheduler->scheduleDeviceRun(&device, 10);
    snes.scheduler->step();

    REQUIRE(device.getTime() == 13);
    REQUIRE(pupsnes::SchedulerTestAccess::hasPendingRun(*snes.scheduler, device.getDeviceId()));
    REQUIRE(pupsnes::SchedulerTestAccess::pendingRunTime(*snes.scheduler, device.getDeviceId()) == 20);
}

TEST_CASE("Scheduler schedules NoWork wakes and deschedules fully when none is provided", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice sleeping(&snes);
    ScriptedDevice idle(&snes);

    sleeping.pushResult({1, pupsnes::TickStopReason::NoWork, 0, 40});
    idle.pushResult({1, pupsnes::TickStopReason::NoWork});

    snes.scheduler->scheduleDeviceRun(&sleeping, 10);
    snes.scheduler->scheduleDeviceRun(&idle, 20);

    snes.scheduler->step();
    REQUIRE(pupsnes::SchedulerTestAccess::pendingRunTime(*snes.scheduler, sleeping.getDeviceId()) == 40);

    snes.scheduler->step();
    REQUIRE_FALSE(pupsnes::SchedulerTestAccess::hasPendingRun(*snes.scheduler, idle.getDeviceId()));
}

TEST_CASE("Scheduler rejects invalid TickResult combinations", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice missing_token(&snes);
    ScriptedDevice past_wake(&snes);

    missing_token.pushResult({0, pupsnes::TickStopReason::BlockedOnToken});
    past_wake.pushResult({2, pupsnes::TickStopReason::ReachedLocalBoundary, 0, 1});

    snes.scheduler->scheduleDeviceRun(&missing_token, 10);
    REQUIRE_THROWS_AS(snes.scheduler->step(), std::logic_error);

    snes.scheduler->scheduleDeviceRun(&past_wake, 10);
    REQUIRE_THROWS_AS(snes.scheduler->step(), std::logic_error);
}

TEST_CASE("Scheduler wakes blocked devices on token completion only when the token is still authoritative", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice blocked(&snes);
    ScriptedDevice replaced(&snes);

    const auto blocked_token =
        snes.scheduler->createToken({pupsnes::TokenType::BusRead, blocked.getDeviceId(), 50, 0x2100, 0});
    const auto replaced_token =
        snes.scheduler->createToken({pupsnes::TokenType::BusRead, replaced.getDeviceId(), 60, 0x2101, 0});

    blocked.pushResult({4, pupsnes::TickStopReason::BlockedOnToken, blocked_token});
    replaced.pushResult({1, pupsnes::TickStopReason::BlockedOnToken, replaced_token});
    replaced.pushResult({1, pupsnes::TickStopReason::NoWork});

    snes.scheduler->scheduleDeviceRun(&blocked, 10);
    snes.scheduler->scheduleDeviceRun(&replaced, 20);

    snes.scheduler->step();
    REQUIRE(pupsnes::SchedulerTestAccess::blockedToken(*snes.scheduler, blocked.getDeviceId()) == blocked_token);

    snes.scheduler->step();
    REQUIRE(pupsnes::SchedulerTestAccess::blockedToken(*snes.scheduler, replaced.getDeviceId()) == replaced_token);

    snes.scheduler->scheduleDeviceRun(&replaced, 70);
    REQUIRE(pupsnes::SchedulerTestAccess::blockedToken(*snes.scheduler, replaced.getDeviceId()) == 0);

    snes.scheduler->step();
    REQUIRE(pupsnes::SchedulerTestAccess::hasPendingRun(*snes.scheduler, blocked.getDeviceId()));
    REQUIRE(pupsnes::SchedulerTestAccess::pendingRunTime(*snes.scheduler, blocked.getDeviceId()) == 50);

    snes.scheduler->step();
    REQUIRE(pupsnes::SchedulerTestAccess::hasPendingRun(*snes.scheduler, replaced.getDeviceId()));
    REQUIRE(pupsnes::SchedulerTestAccess::pendingRunTime(*snes.scheduler, replaced.getDeviceId()) == 70);
}

TEST_CASE("Token completion can be observed on a later natural run without an automatic wake", "[unit]") {
    pupsnes::SNES snes;
    DynamicTokenObserverDevice device(&snes);

    const auto token_id = snes.scheduler->createToken({pupsnes::TokenType::BusRead, device.getDeviceId(), 50, 0x2100, 0});
    device.watched_token = token_id;

    snes.scheduler->scheduleDeviceRun(&device, 10);

    snes.scheduler->step();
    REQUIRE_FALSE(device.saw_completed_token);

    snes.scheduler->scheduleDeviceRun(&device, 60);

    snes.scheduler->step();
    REQUIRE(snes.getMasterTime() == 50);

    snes.scheduler->step();
    REQUIRE(device.tick_calls == 2);
    REQUIRE(device.saw_completed_token);
}

TEST_CASE("Authoritative run replacement prefers the newest wake in both directions", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice earlier(&snes);
    ScriptedDevice later(&snes);

    earlier.pushResult({1, pupsnes::TickStopReason::NoWork});
    snes.scheduler->scheduleDeviceRun(&earlier, 20);
    const auto old_generation = pupsnes::SchedulerTestAccess::runGeneration(*snes.scheduler, earlier.getDeviceId());
    snes.scheduler->scheduleDeviceRun(&earlier, 10);
    const auto new_generation = pupsnes::SchedulerTestAccess::runGeneration(*snes.scheduler, earlier.getDeviceId());

    REQUIRE(new_generation > old_generation);
    REQUIRE(pupsnes::SchedulerTestAccess::pendingRunTime(*snes.scheduler, earlier.getDeviceId()) == 10);

    snes.scheduler->step();
    REQUIRE(earlier.tick_calls == 1);
    REQUIRE(snes.getMasterTime() == 10);

    snes.scheduler->step();
    REQUIRE(earlier.tick_calls == 1);

    later.pushResult({1, pupsnes::TickStopReason::NoWork});
    snes.scheduler->scheduleDeviceRun(&later, 10);
    snes.scheduler->scheduleDeviceRun(&later, 30);

    snes.scheduler->step();
    REQUIRE(later.tick_calls == 1);
    REQUIRE(snes.getMasterTime() == 30);
}

TEST_CASE("Only one authoritative pending run exists even if stale queue entries remain", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice device(&snes);

    snes.scheduler->scheduleDeviceRun(&device, 20);
    snes.scheduler->scheduleDeviceRun(&device, 10);
    snes.scheduler->scheduleDeviceRun(&device, 15);

    REQUIRE(pupsnes::SchedulerTestAccess::hasPendingRun(*snes.scheduler, device.getDeviceId()));
    REQUIRE(pupsnes::SchedulerTestAccess::pendingRunTime(*snes.scheduler, device.getDeviceId()) == 15);
    REQUIRE(pupsnes::SchedulerTestAccess::queuedRunEventsFor(*snes.scheduler, device.getDeviceId()) == 3);
}

TEST_CASE("Scheduler fails loudly on repeated same-time zero-progress runs", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice device(&snes);

    device.pushResult({0, pupsnes::TickStopReason::BudgetExhausted});
    device.pushResult({0, pupsnes::TickStopReason::BudgetExhausted});

    snes.scheduler->scheduleDeviceRun(&device, 10);

    snes.scheduler->step();
    REQUIRE(device.tick_calls == 1);

    REQUIRE_THROWS_AS(snes.scheduler->step(), std::logic_error);
}

TEST_CASE("Stale run event at heap head does not cap budget for the next real event", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice device(&snes);
    ScriptedDevice other(&snes);

    // Schedule device at time 3, then replace with time 8. The stale event at 3 stays in the heap.
    snes.scheduler->scheduleDeviceRun(&device, 3);
    snes.scheduler->scheduleDeviceRun(&device, 8);

    // Schedule other at time 0 so it runs first.
    other.pushResult({1, pupsnes::TickStopReason::BudgetExhausted});
    snes.scheduler->scheduleDeviceRun(&other, 0);

    // other runs at time 0. If the stale event at 3 were used, budget would be 3.
    // The real next event is device at 8, so budget should be min(MAX_CYCLES_STEP, 8) = 8.
    snes.scheduler->step();
    REQUIRE(other.tick_calls == 1);
    REQUIRE(other.budgets.at(0) == 8);
}

TEST_CASE("catchUpDevice keeps iterating until the target time is reached", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice device(&snes);

    device.pushResult({2, pupsnes::TickStopReason::BudgetExhausted});
    device.pushResult({3, pupsnes::TickStopReason::BudgetExhausted});

    snes.scheduler->catchUpDevice(device.getDeviceId(), 5);

    REQUIRE(device.tick_calls == 2);
    REQUIRE(device.start_times == std::vector<pupsnes::time_master_t>{0, 2});
    REQUIRE(device.getTime() == 5);
}

TEST_CASE("catchUpDevice continues across local boundaries whose wakes are within the target", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice device(&snes);

    device.pushResult({2, pupsnes::TickStopReason::ReachedLocalBoundary, 0, 4});
    device.pushResult({2, pupsnes::TickStopReason::BudgetExhausted});

    snes.scheduler->catchUpDevice(device.getDeviceId(), 6);

    REQUIRE(device.tick_calls == 2);
    REQUIRE(device.start_times == std::vector<pupsnes::time_master_t>{0, 4});
    REQUIRE(device.getTime() == 6);
}

TEST_CASE("catchUpDevice rejects invalid synchronous stop reasons", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice no_work(&snes);
    ScriptedDevice blocked(&snes);

    no_work.pushResult({1, pupsnes::TickStopReason::NoWork});
    blocked.pushResult({1, pupsnes::TickStopReason::BlockedOnToken, 7});

    REQUIRE_THROWS_AS(snes.scheduler->catchUpDevice(no_work.getDeviceId(), 3), std::logic_error);
    REQUIRE_THROWS_AS(snes.scheduler->catchUpDevice(blocked.getDeviceId(), 3), std::logic_error);
}

TEST_CASE("catchUpDevice rejects a device that is blocked on a token", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice device(&snes);

    // First run: device blocks on a token.
    device.pushResult({2, pupsnes::TickStopReason::BlockedOnToken, 99});
    snes.scheduler->scheduleDeviceRun(&device, 0);
    snes.scheduler->step();
    REQUIRE(pupsnes::SchedulerTestAccess::blockedToken(*snes.scheduler, device.getDeviceId()) == 99);

    // Attempting catch-up while blocked must fail.
    REQUIRE_THROWS_AS(snes.scheduler->catchUpDevice(device.getDeviceId(), 10), std::logic_error);
}

TEST_CASE("catchUpDevice accepts NoWork when the device has reached the target", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice device(&snes);

    // Device consumes exactly the budget and reports NoWork with a future wake.
    device.pushResult({5, pupsnes::TickStopReason::NoWork, 0, 20});

    snes.scheduler->catchUpDevice(device.getDeviceId(), 5);

    REQUIRE(device.tick_calls == 1);
    REQUIRE(device.getTime() == 5);
    // handleRunResult should have scheduled a run at the wake time.
    REQUIRE(pupsnes::SchedulerTestAccess::hasPendingRun(*snes.scheduler, device.getDeviceId()));
    REQUIRE(pupsnes::SchedulerTestAccess::pendingRunTime(*snes.scheduler, device.getDeviceId()) == 20);
}

TEST_CASE("catchUpDevice rejects ReachedLocalBoundary with wake beyond the target", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice device(&snes);

    // Device stops at a local boundary before the target, but next_wake_time exceeds target.
    device.pushResult({2, pupsnes::TickStopReason::ReachedLocalBoundary, 0, 10});

    REQUIRE_THROWS_AS(snes.scheduler->catchUpDevice(device.getDeviceId(), 5), std::logic_error);
}

TEST_CASE("catchUpDevice with NoWork at target and no wake fully deschedules the device", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice device(&snes);

    // Device consumes exactly the budget and reports NoWork without a wake time.
    device.pushResult({5, pupsnes::TickStopReason::NoWork});

    snes.scheduler->catchUpDevice(device.getDeviceId(), 5);

    REQUIRE(device.tick_calls == 1);
    REQUIRE(device.getTime() == 5);
    REQUIRE_FALSE(pupsnes::SchedulerTestAccess::hasPendingRun(*snes.scheduler, device.getDeviceId()));
}

TEST_CASE("catchUpDevice with ReachedLocalBoundary exactly at target succeeds and schedules wake", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice device(&snes);

    // Device reaches target exactly and reports a local boundary with a future wake.
    device.pushResult({5, pupsnes::TickStopReason::ReachedLocalBoundary, 0, 20});

    snes.scheduler->catchUpDevice(device.getDeviceId(), 5);

    REQUIRE(device.tick_calls == 1);
    REQUIRE(device.getTime() == 5);
    REQUIRE(pupsnes::SchedulerTestAccess::hasPendingRun(*snes.scheduler, device.getDeviceId()));
    REQUIRE(pupsnes::SchedulerTestAccess::pendingRunTime(*snes.scheduler, device.getDeviceId()) == 20);
}

TEST_CASE("Scheduler createToken returns a valid token and schedules CommitComplete", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice device(&snes);

    const auto token_id = snes.scheduler->createToken({pupsnes::TokenType::BusRead, device.getDeviceId(), 50, 0x2100, 0});

    const auto *token = snes.scheduler->getToken(token_id);
    REQUIRE(token != nullptr);
    REQUIRE(token->type == pupsnes::TokenType::BusRead);
    REQUIRE(token->completion_time == 50);

    const auto event = pupsnes::SchedulerTestAccess::peekNextEvent(*snes.scheduler);
    REQUIRE(event.time == 50);
    REQUIRE(event.subphase == pupsnes::SchedulerPhase::CommitComplete);
}

TEST_CASE("Scheduler rejects past events", "[unit]") {
    pupsnes::SNES snes;
    ScriptedDevice device(&snes);

    snes.setMasterTime(100);
    snes.scheduler->scheduleEvent(50, &device, pupsnes::SchedulerPhase::WakeSample, pupsnes::EventType::DeviceBoundary);

    REQUIRE_THROWS_AS(snes.scheduler->step(), std::logic_error);
}
