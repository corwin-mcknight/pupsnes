#include <catch2/catch_test_macros.hpp>

#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/token.h"
#include "scheduler_test_access.h"

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

    void onEvent(const pupsnes::SchedulerEvent &) override { ++event_calls; }

    int tick_calls = 0;
    int event_calls = 0;
    pupsnes::time_master_delta_t last_budget = 0;
};
} // namespace

TEST_CASE("SNES assigns stable device IDs", "[unit]") {
    pupsnes::SNES snes;
    FakeDevice dev1(&snes);
    FakeDevice dev2(&snes);
    FakeDevice dev3(&snes);

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
    REQUIRE(snes.getMasterTime() == 0);

    snes.scheduler->step();

    REQUIRE(snes.getMasterTime() == 5);
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
        REQUIRE(snes.getMasterTime() == static_cast<pupsnes::time_master_t>((i + 1) * 10));
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
    REQUIRE(snes.getMasterTime() == T);
    REQUIRE(pupsnes::SchedulerTestAccess::eventQueueSize(*snes.scheduler) == 0);
    for (std::size_t i = 0; i < N; ++i) {
        REQUIRE(devices[i]->tick_calls == 1);
    }
    // First N-1 devices get budget=0 because next event is at the same time.
    for (std::size_t i = 0; i < N - 1; ++i) {
        REQUIRE(devices[i]->last_budget == 0);
    }
    // Last device has no successor, so it gets MAX_CYCLES_STEP.
    REQUIRE(devices[N - 1]->last_budget == pupsnes::Scheduler::MAX_CYCLES_STEP);
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

TEST_CASE("Scheduler updates device local_time after tick", "[unit]") {
    pupsnes::SNES snes;
    FakeDevice device(&snes);

    REQUIRE(device.getTime() == 0);

    snes.scheduler->scheduleEvent(10, &device, pupsnes::SchedulerPhase::Run,
                                  pupsnes::EventType::DeviceRun);
    snes.scheduler->step();

    // FakeDevice returns {budget, BudgetExhausted}, so local_time should advance by budget.
    REQUIRE(device.getTime() == device.last_budget);
    REQUIRE(device.getTime() > 0);
}

namespace {
class BlockingDevice : public pupsnes::Device {
  public:
    explicit BlockingDevice(pupsnes::SNES *snes) : pupsnes::Device(snes) {}

    pupsnes::TickResult tick(pupsnes::time_master_delta_t budget) override {
        ++tick_calls;
        last_budget = budget;
        if (should_block && !has_blocked) {
            has_blocked = true;
            return {consumed_before_block, pupsnes::TickStopReason::BlockedOnToken, block_token};
        }
        return {budget, pupsnes::TickStopReason::BudgetExhausted};
    }

    void onEvent(const pupsnes::SchedulerEvent &) override { ++event_calls; }

    int tick_calls = 0;
    int event_calls = 0;
    pupsnes::time_master_delta_t last_budget = 0;
    bool should_block = false;
    bool has_blocked = false;
    pupsnes::token_id_t block_token = 0;
    pupsnes::time_master_delta_t consumed_before_block = 0;
};
} // namespace

TEST_CASE("Scheduler wakes device blocked on token after CommitComplete resolves it", "[unit]") {
    pupsnes::SNES snes;
    BlockingDevice device(&snes);

    auto token_id = snes.scheduler->createToken(pupsnes::TokenType::BusRead,
                                                device.getDeviceId(), 50, 0x2100, 0);

    snes.scheduler->scheduleEvent(10, &device, pupsnes::SchedulerPhase::Run,
                                  pupsnes::EventType::DeviceRun);

    device.should_block = true;
    device.block_token = token_id;
    device.consumed_before_block = 4;

    // Step 1: Device runs at time 10, returns BlockedOnToken
    snes.scheduler->step();
    REQUIRE(device.tick_calls == 1);
    REQUIRE(snes.getMasterTime() == 10);

    // Step 2: CommitComplete fires at time 50, resolves token, auto-schedules Run
    snes.scheduler->step();
    REQUIRE(snes.getMasterTime() == 50);

    // Step 3: The auto-scheduled Run event fires, device runs again
    snes.scheduler->step();
    REQUIRE(device.tick_calls == 2);

    auto *token = snes.scheduler->getToken(token_id);
    REQUIRE(token != nullptr);
    REQUIRE(token->state == pupsnes::TokenState::Completed);
}

namespace {
class PollingDevice : public pupsnes::Device {
  public:
    explicit PollingDevice(pupsnes::SNES *snes) : pupsnes::Device(snes) {}

    pupsnes::TickResult tick(pupsnes::time_master_delta_t budget) override {
        ++tick_calls;
        if (has_poll_token) {
            auto *token = snes->scheduler->getToken(poll_token);
            if (token != nullptr && token->state == pupsnes::TokenState::Completed) {
                read_data = token->data;
                token_was_ready = true;
            }
        }
        return {budget, pupsnes::TickStopReason::BudgetExhausted};
    }

    void onEvent(const pupsnes::SchedulerEvent &) override {}

    int tick_calls = 0;
    bool has_poll_token = false;
    pupsnes::token_id_t poll_token = 0;
    uint8_t read_data = 0;
    bool token_was_ready = false;
};
} // namespace

TEST_CASE("Device can poll token state on natural clock cycle", "[unit]") {
    pupsnes::SNES snes;
    PollingDevice device(&snes);

    auto token_id = snes.scheduler->createToken(pupsnes::TokenType::BusRead,
                                                device.getDeviceId(), 50, 0x2100, 0);

    device.poll_token = token_id;
    device.has_poll_token = true;

    // Run at time 10 — before token completes
    snes.scheduler->scheduleEvent(10, &device, pupsnes::SchedulerPhase::Run,
                                  pupsnes::EventType::DeviceRun);
    snes.scheduler->step();
    REQUIRE(device.tick_calls == 1);
    REQUIRE_FALSE(device.token_was_ready);

    // CommitComplete at 50 resolves the token
    snes.scheduler->scheduleEvent(60, &device, pupsnes::SchedulerPhase::Run,
                                  pupsnes::EventType::DeviceRun);
    snes.scheduler->step();
    REQUIRE(snes.getMasterTime() == 50);

    // Device runs at 60, polls and sees completed token
    snes.scheduler->step();
    REQUIRE(device.tick_calls == 2);
    REQUIRE(device.token_was_ready);
}

TEST_CASE("Scheduler createToken returns valid token and schedules CommitComplete", "[unit]") {
    pupsnes::SNES snes;
    FakeDevice device(&snes);

    auto token_id = snes.scheduler->createToken(pupsnes::TokenType::BusRead, device.getDeviceId(),
                                                50, 0x2100, 0);

    auto *token = snes.scheduler->getToken(token_id);
    REQUIRE(token != nullptr);
    REQUIRE(token->type == pupsnes::TokenType::BusRead);
    REQUIRE(token->completion_time == 50);

    auto event = pupsnes::SchedulerTestAccess::peekNextEvent(*snes.scheduler);
    REQUIRE(event.time == 50);
    REQUIRE(event.subphase == pupsnes::SchedulerPhase::CommitComplete);
}

TEST_CASE("Scheduler does not tick device for past events", "[unit]") {
    pupsnes::SNES snes;
    FakeDevice device(&snes);

    // Advance time past where the event is scheduled.
    snes.setMasterTime(100);
    snes.scheduler->scheduleEvent(50, &device, pupsnes::SchedulerPhase::Run,
                                  pupsnes::EventType::DeviceRun);

    // In debug builds the assert fires before we get here.
    // In release builds (NDEBUG), the event is skipped via early return.
    snes.scheduler->step();

    REQUIRE(device.tick_calls == 0);
    REQUIRE(device.event_calls == 0);
}
