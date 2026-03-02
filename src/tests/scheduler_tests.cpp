#include <catch2/catch_test_macros.hpp>

#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"

#include <memory>
#include <vector>

namespace {
class FakeDevice : public pupsnes::Device {
  public:
    explicit FakeDevice(pupsnes::SNES *snes) : pupsnes::Device(snes) {}

    pupsnes::TickResult tick(pupsnes::time_master_delta_t budget) override {
        ++tick_calls;
        last_budget = budget;
        return {budget, pupsnes::TickStopReason::BudgetExhausted};
    }

    void on_event(const pupsnes::SchedulerEvent &) override { ++event_calls; }

    int tick_calls = 0;
    int event_calls = 0;
    pupsnes::time_master_delta_t last_budget = 0;
};
} // namespace

TEST_CASE("Scheduler orders events by time, subphase, type, and seq", "[unit]") {
    pupsnes::Scheduler scheduler(nullptr);
    FakeDevice device(nullptr);

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
    FakeDevice device(nullptr);

    scheduler.scheduleEvent(3, &device, pupsnes::SchedulerPhase::Run, pupsnes::EventType::DeviceRun);
    scheduler.scheduleEvent(3, &device, pupsnes::SchedulerPhase::Run, pupsnes::EventType::DeviceRun);

    auto first = pupsnes::SchedulerTestAccess::popNextEvent(scheduler);
    auto second = pupsnes::SchedulerTestAccess::popNextEvent(scheduler);

    REQUIRE(first.seq < second.seq);
}

TEST_CASE("Scheduler step advances time and consumes the next event", "[unit]") {
    pupsnes::SNES snes;
    FakeDevice device(&snes);

    snes.scheduler->scheduleEvent(5, &device, pupsnes::SchedulerPhase::Run, pupsnes::EventType::DeviceRun);

    REQUIRE(pupsnes::SchedulerTestAccess::eventQueueSize(*snes.scheduler) == 1);
    REQUIRE(snes.time_now == 0);

    snes.scheduler->step();

    REQUIRE(snes.time_now == 5);
    REQUIRE(pupsnes::SchedulerTestAccess::eventQueueSize(*snes.scheduler) == 0);
}

TEST_CASE("Scheduler step runs devices scheduled for Run", "[unit]") {
    pupsnes::SNES snes;
    FakeDevice device(&snes);

    snes.scheduler->scheduleEvent(4, &device, pupsnes::SchedulerPhase::Run, pupsnes::EventType::DeviceRun);

    snes.scheduler->step();

    REQUIRE(device.tick_calls >= 1);
    REQUIRE(device.last_budget > 0);
}

TEST_CASE("Scheduler processes 10 DeviceRun events in chronological order", "[unit]") {
    pupsnes::SNES snes;
    constexpr std::size_t N = 10;

    std::vector<std::unique_ptr<FakeDevice>> devices;
    devices.reserve(N);
    for (std::size_t i = 0; i < N; ++i) {
        devices.push_back(std::make_unique<FakeDevice>(&snes));
    }

    for (std::size_t i = 0; i < N; ++i) {
        snes.scheduler->scheduleEvent(static_cast<pupsnes::time_master_t>((i + 1) * 10),
                                      devices[i].get(), pupsnes::SchedulerPhase::Run,
                                      pupsnes::EventType::DeviceRun);
    }

    REQUIRE(pupsnes::SchedulerTestAccess::eventQueueSize(*snes.scheduler) == N);

    for (std::size_t i = 0; i < N; ++i) {
        snes.scheduler->step();
        // Time must advance to each event's timestamp.
        REQUIRE(snes.time_now == static_cast<pupsnes::time_master_t>((i + 1) * 10));
        // Only the device whose event was just processed should have been ticked so far.
        REQUIRE(devices[i]->tick_calls == 1);
    }

    REQUIRE(pupsnes::SchedulerTestAccess::eventQueueSize(*snes.scheduler) == 0);
}

TEST_CASE("Scheduler budget is capped by next event time with 10 devices", "[unit]") {
    pupsnes::SNES snes;
    constexpr std::size_t N = 10;
    // Spacing smaller than MAX_CYCLES_STEP so the cap is exercised.
    constexpr pupsnes::time_master_t spacing = 3;

    std::vector<std::unique_ptr<FakeDevice>> devices;
    devices.reserve(N);
    for (std::size_t i = 0; i < N; ++i) {
        devices.push_back(std::make_unique<FakeDevice>(&snes));
    }

    for (std::size_t i = 0; i < N; ++i) {
        snes.scheduler->scheduleEvent(static_cast<pupsnes::time_master_t>((i + 1) * spacing),
                                      devices[i].get(), pupsnes::SchedulerPhase::Run,
                                      pupsnes::EventType::DeviceRun);
    }

    for (std::size_t i = 0; i < N; ++i) {
        snes.scheduler->step();
    }

    // The first N-1 devices must have been capped at `spacing` cycles.
    for (std::size_t i = 0; i < N - 1; ++i) {
        REQUIRE(devices[i]->tick_calls == 1);
        REQUIRE(devices[i]->last_budget == spacing);
    }
    // The last device has no successor in the queue, so it gets the full MAX_CYCLES_STEP.
    REQUIRE(devices[N - 1]->tick_calls == 1);
    REQUIRE(devices[N - 1]->last_budget == pupsnes::Scheduler::MAX_CYCLES_STEP);
}

TEST_CASE("Scheduler processes 10 DeviceRun events at the same timestamp", "[unit]") {
    pupsnes::SNES snes;
    constexpr std::size_t N = 10;
    constexpr pupsnes::time_master_t T = 42;

    std::vector<std::unique_ptr<FakeDevice>> devices;
    devices.reserve(N);
    for (std::size_t i = 0; i < N; ++i) {
        devices.push_back(std::make_unique<FakeDevice>(&snes));
    }

    for (std::size_t i = 0; i < N; ++i) {
        snes.scheduler->scheduleEvent(T, devices[i].get(), pupsnes::SchedulerPhase::Run,
                                      pupsnes::EventType::DeviceRun);
    }

    for (std::size_t i = 0; i < N; ++i) {
        snes.scheduler->step();
    }

    // All same-time Run events must be dispatched and time must stay at T.
    REQUIRE(snes.time_now == T);
    REQUIRE(pupsnes::SchedulerTestAccess::eventQueueSize(*snes.scheduler) == 0);
    for (std::size_t i = 0; i < N; ++i) {
        REQUIRE(devices[i]->tick_calls == 1);
        // Same-time Run events are not hazards to each other (CommitComplete/WakeSample
        // already settled via subphase ordering), so each device still gets full budget.
        REQUIRE(devices[i]->last_budget == pupsnes::Scheduler::MAX_CYCLES_STEP);
    }
}

TEST_CASE("Scheduler dispatches CommitComplete and WakeSample to on_event for 10 devices",
          "[unit]") {
    pupsnes::SNES snes;
    constexpr std::size_t N = 10;

    std::vector<std::unique_ptr<FakeDevice>> devices;
    devices.reserve(N);
    for (std::size_t i = 0; i < N; ++i) {
        devices.push_back(std::make_unique<FakeDevice>(&snes));
    }

    // Assign phases in rotation: CommitComplete, WakeSample, Run, repeat.
    for (std::size_t i = 0; i < N; ++i) {
        pupsnes::SchedulerPhase phase;
        if (i % 3 == 0) {
            phase = pupsnes::SchedulerPhase::CommitComplete;
        } else if (i % 3 == 1) {
            phase = pupsnes::SchedulerPhase::WakeSample;
        } else {
            phase = pupsnes::SchedulerPhase::Run;
        }
        snes.scheduler->scheduleEvent(static_cast<pupsnes::time_master_t>(10 + i),
                                      devices[i].get(), phase, pupsnes::EventType::DeviceRun);
    }

    for (std::size_t i = 0; i < N; ++i) {
        snes.scheduler->step();
    }

    REQUIRE(pupsnes::SchedulerTestAccess::eventQueueSize(*snes.scheduler) == 0);

    for (std::size_t i = 0; i < N; ++i) {
        if (i % 3 == 2) {
            // Run phase: tick() must be called, on_event() must not.
            REQUIRE(devices[i]->tick_calls == 1);
            REQUIRE(devices[i]->event_calls == 0);
        } else {
            // CommitComplete / WakeSample: on_event() must be called, tick() must not.
            REQUIRE(devices[i]->event_calls == 1);
            REQUIRE(devices[i]->tick_calls == 0);
        }
    }
}
