#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/token.h"
#include "scheduler_test_access.h"

namespace {

class ScriptedDevice : public pupsnes::Device {
 public:
  using TickScript = std::function<pupsnes::TickResult(ScriptedDevice&, pupsnes::TimeMasterDeltaT)>;

  explicit ScriptedDevice(pupsnes::SNES* snes) : pupsnes::Device(snes) {}

  pupsnes::TickResult Tick(pupsnes::TimeMasterDeltaT budget) override {
    ++tick_calls;
    start_times.push_back(GetTime());
    budgets.push_back(budget);

    if (scripts_.empty()) {
      return {budget, pupsnes::TickStopReason::kBudgetExhausted};
    }

    TickScript script = scripts_.front();
    scripts_.pop_front();
    return script(*this, budget);
  }

  void OnEvent(const pupsnes::SchedulerEvent& event) override {
    ++event_calls;
    seen_events.push_back(event);
  }

  void PushScript(TickScript script) { scripts_.push_back(std::move(script)); }

  void PushResult(const pupsnes::TickResult& result) {
    scripts_.push_back([result](ScriptedDevice&, pupsnes::TimeMasterDeltaT) { return result; });
  }

  int tick_calls = 0;
  int event_calls = 0;
  std::vector<pupsnes::TimeMasterT> start_times;
  std::vector<pupsnes::TimeMasterDeltaT> budgets;
  std::vector<pupsnes::SchedulerEvent> seen_events;

 private:
  std::deque<TickScript> scripts_;
};

class DynamicTokenObserverDevice : public pupsnes::Device {
 public:
  explicit DynamicTokenObserverDevice(pupsnes::SNES* snes) : pupsnes::Device(snes) {}

  pupsnes::TickResult Tick(pupsnes::TimeMasterDeltaT budget) override {
    ++tick_calls;
    last_budget = budget;
    if (watched_token != 0) {
      const auto* token = snes_->scheduler->GetToken(watched_token);
      saw_completed_token = token != nullptr && token->state == pupsnes::TokenState::kCompleted;
    }
    return {budget, pupsnes::TickStopReason::kNoWork};
  }

  void OnEvent(const pupsnes::SchedulerEvent&) override {}

  pupsnes::TokenIdT watched_token = 0;
  bool saw_completed_token = false;
  int tick_calls = 0;
  pupsnes::TimeMasterDeltaT last_budget = 0;
};

}  // namespace

TEST_CASE("SNES assigns stable device IDs", "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice dev1(&snes);
  ScriptedDevice dev2(&snes);
  ScriptedDevice dev3(&snes);

  REQUIRE(dev2.GetDeviceId() == dev1.GetDeviceId() + 1);
  REQUIRE(dev3.GetDeviceId() == dev2.GetDeviceId() + 1);

  REQUIRE(snes.GetDevice(dev1.GetDeviceId()) == &dev1);
  REQUIRE(snes.GetDevice(dev2.GetDeviceId()) == &dev2);
  REQUIRE(snes.GetDevice(dev3.GetDeviceId()) == &dev3);
  REQUIRE(snes.GetDevice(99) == nullptr);
}

TEST_CASE("Scheduler orders events by time, subphase, type, and seq", "[unit]") {
  pupsnes::Scheduler scheduler(nullptr);
  ScriptedDevice device(nullptr);

  scheduler.ScheduleEvent(5, &device, pupsnes::SchedulerPhase::kRun, pupsnes::EventType::kDeviceBoundary);
  scheduler.ScheduleEvent(5, &device, pupsnes::SchedulerPhase::kRun, pupsnes::EventType::kDeviceRun);
  scheduler.ScheduleEvent(5, &device, pupsnes::SchedulerPhase::kCommitComplete, pupsnes::EventType::kDeviceRun);
  scheduler.ScheduleEvent(2, &device, pupsnes::SchedulerPhase::kRun, pupsnes::EventType::kDeviceRun);

  REQUIRE(pupsnes::SchedulerTestAccess::EventQueueSize(scheduler) == 4);

  auto first = pupsnes::SchedulerTestAccess::PopNextEvent(scheduler);
  auto second = pupsnes::SchedulerTestAccess::PopNextEvent(scheduler);
  auto third = pupsnes::SchedulerTestAccess::PopNextEvent(scheduler);
  auto fourth = pupsnes::SchedulerTestAccess::PopNextEvent(scheduler);

  REQUIRE(first.time == 2);
  REQUIRE(second.subphase == pupsnes::SchedulerPhase::kCommitComplete);
  REQUIRE(third.type == pupsnes::EventType::kDeviceRun);
  REQUIRE(fourth.type == pupsnes::EventType::kDeviceBoundary);
}

TEST_CASE("Scheduler uses sequence as final tiebreaker", "[unit]") {
  pupsnes::Scheduler scheduler(nullptr);
  ScriptedDevice device(nullptr);

  scheduler.ScheduleEvent(3, &device, pupsnes::SchedulerPhase::kRun, pupsnes::EventType::kDeviceRun);
  scheduler.ScheduleEvent(3, &device, pupsnes::SchedulerPhase::kRun, pupsnes::EventType::kDeviceRun);

  auto first = pupsnes::SchedulerTestAccess::PopNextEvent(scheduler);
  auto second = pupsnes::SchedulerTestAccess::PopNextEvent(scheduler);

  REQUIRE(first.seq < second.seq);
}

TEST_CASE("Scheduler step advances time and dispatches non-run events", "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice device(&snes);

  snes.scheduler->ScheduleEvent(5, &device, pupsnes::SchedulerPhase::kWakeSample, pupsnes::EventType::kDeviceBoundary);

  snes.scheduler->Step();

  REQUIRE(snes.GetMasterTime() == 5);
  REQUIRE(device.event_calls == 1);
  REQUIRE(device.tick_calls == 0);
}

TEST_CASE(
    "Scheduler aligns device time to its wake and commits completed cycles "
    "exactly once",
    "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice device(&snes);

  device.PushResult({3, pupsnes::TickStopReason::kNoWork});
  snes.scheduler->ScheduleDeviceRun(&device, 10);
  snes.scheduler->Step();

  REQUIRE(device.tick_calls == 1);
  REQUIRE(device.start_times.at(0) == 10);
  REQUIRE(device.GetTime() == 13);
  REQUIRE_FALSE(pupsnes::SchedulerTestAccess::HasPendingRun(*snes.scheduler, device.GetDeviceId()));
}

TEST_CASE("Scheduler reschedules BudgetExhausted devices at their committed time", "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice device(&snes);

  device.PushResult({2, pupsnes::TickStopReason::kBudgetExhausted});
  snes.scheduler->ScheduleDeviceRun(&device, 10);
  snes.scheduler->Step();

  REQUIRE(device.GetTime() == 12);
  REQUIRE(pupsnes::SchedulerTestAccess::HasPendingRun(*snes.scheduler, device.GetDeviceId()));
  REQUIRE(pupsnes::SchedulerTestAccess::PendingRunTime(*snes.scheduler, device.GetDeviceId()) == 12);
}

TEST_CASE("Scheduler schedules local-boundary wakes authoritatively", "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice device(&snes);

  device.PushResult({3, pupsnes::TickStopReason::kReachedLocalBoundary, 0, 20});
  snes.scheduler->ScheduleDeviceRun(&device, 10);
  snes.scheduler->Step();

  REQUIRE(device.GetTime() == 13);
  REQUIRE(pupsnes::SchedulerTestAccess::HasPendingRun(*snes.scheduler, device.GetDeviceId()));
  REQUIRE(pupsnes::SchedulerTestAccess::PendingRunTime(*snes.scheduler, device.GetDeviceId()) == 20);
}

TEST_CASE(
    "Scheduler schedules NoWork wakes and deschedules fully when none is "
    "provided",
    "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice sleeping(&snes);
  ScriptedDevice idle(&snes);

  sleeping.PushResult({1, pupsnes::TickStopReason::kNoWork, 0, 40});
  idle.PushResult({1, pupsnes::TickStopReason::kNoWork});

  snes.scheduler->ScheduleDeviceRun(&sleeping, 10);
  snes.scheduler->ScheduleDeviceRun(&idle, 20);

  snes.scheduler->Step();
  REQUIRE(pupsnes::SchedulerTestAccess::PendingRunTime(*snes.scheduler, sleeping.GetDeviceId()) == 40);

  snes.scheduler->Step();
  REQUIRE_FALSE(pupsnes::SchedulerTestAccess::HasPendingRun(*snes.scheduler, idle.GetDeviceId()));
}

TEST_CASE("Scheduler rejects invalid TickResult combinations", "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice missing_token(&snes);
  ScriptedDevice past_wake(&snes);

  missing_token.PushResult({0, pupsnes::TickStopReason::kBlockedOnToken});
  past_wake.PushResult({2, pupsnes::TickStopReason::kReachedLocalBoundary, 0, 1});

  snes.scheduler->ScheduleDeviceRun(&missing_token, 10);
  REQUIRE_THROWS_AS(snes.scheduler->Step(), std::logic_error);

  snes.scheduler->ScheduleDeviceRun(&past_wake, 10);
  REQUIRE_THROWS_AS(snes.scheduler->Step(), std::logic_error);
}

TEST_CASE(
    "Scheduler wakes blocked devices on token completion only when the token "
    "is still authoritative",
    "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice blocked(&snes);
  ScriptedDevice replaced(&snes);

  const auto blocked_token =
      snes.scheduler->CreateToken({pupsnes::TokenType::kBusRead, blocked.GetDeviceId(), 50, 0x2100, 0});
  const auto replaced_token =
      snes.scheduler->CreateToken({pupsnes::TokenType::kBusRead, replaced.GetDeviceId(), 60, 0x2101, 0});

  blocked.PushResult({4, pupsnes::TickStopReason::kBlockedOnToken, blocked_token});
  replaced.PushResult({1, pupsnes::TickStopReason::kBlockedOnToken, replaced_token});
  replaced.PushResult({1, pupsnes::TickStopReason::kNoWork});

  snes.scheduler->ScheduleDeviceRun(&blocked, 10);
  snes.scheduler->ScheduleDeviceRun(&replaced, 20);

  snes.scheduler->Step();
  REQUIRE(pupsnes::SchedulerTestAccess::BlockedToken(*snes.scheduler, blocked.GetDeviceId()) == blocked_token);

  snes.scheduler->Step();
  REQUIRE(pupsnes::SchedulerTestAccess::BlockedToken(*snes.scheduler, replaced.GetDeviceId()) == replaced_token);

  snes.scheduler->ScheduleDeviceRun(&replaced, 70);
  REQUIRE(pupsnes::SchedulerTestAccess::BlockedToken(*snes.scheduler, replaced.GetDeviceId()) == 0);

  snes.scheduler->Step();
  REQUIRE(pupsnes::SchedulerTestAccess::HasPendingRun(*snes.scheduler, blocked.GetDeviceId()));
  REQUIRE(pupsnes::SchedulerTestAccess::PendingRunTime(*snes.scheduler, blocked.GetDeviceId()) == 50);

  snes.scheduler->Step();
  REQUIRE(pupsnes::SchedulerTestAccess::HasPendingRun(*snes.scheduler, replaced.GetDeviceId()));
  REQUIRE(pupsnes::SchedulerTestAccess::PendingRunTime(*snes.scheduler, replaced.GetDeviceId()) == 70);
}

TEST_CASE(
    "Token completion can be observed on a later natural run without an "
    "automatic wake",
    "[unit]") {
  pupsnes::SNES snes;
  DynamicTokenObserverDevice device(&snes);

  const auto token_id =
      snes.scheduler->CreateToken({pupsnes::TokenType::kBusRead, device.GetDeviceId(), 50, 0x2100, 0});
  device.watched_token = token_id;

  snes.scheduler->ScheduleDeviceRun(&device, 10);

  snes.scheduler->Step();
  REQUIRE_FALSE(device.saw_completed_token);

  snes.scheduler->ScheduleDeviceRun(&device, 60);

  snes.scheduler->Step();
  REQUIRE(snes.GetMasterTime() == 50);

  snes.scheduler->Step();
  REQUIRE(device.tick_calls == 2);
  REQUIRE(device.saw_completed_token);
}

TEST_CASE("Authoritative run replacement prefers the newest wake in both directions", "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice earlier(&snes);
  ScriptedDevice later(&snes);

  earlier.PushResult({1, pupsnes::TickStopReason::kNoWork});
  snes.scheduler->ScheduleDeviceRun(&earlier, 20);
  const auto old_generation = pupsnes::SchedulerTestAccess::RunGeneration(*snes.scheduler, earlier.GetDeviceId());
  snes.scheduler->ScheduleDeviceRun(&earlier, 10);
  const auto new_generation = pupsnes::SchedulerTestAccess::RunGeneration(*snes.scheduler, earlier.GetDeviceId());

  REQUIRE(new_generation > old_generation);
  REQUIRE(pupsnes::SchedulerTestAccess::PendingRunTime(*snes.scheduler, earlier.GetDeviceId()) == 10);

  snes.scheduler->Step();
  REQUIRE(earlier.tick_calls == 1);
  REQUIRE(snes.GetMasterTime() == 10);

  snes.scheduler->Step();
  REQUIRE(earlier.tick_calls == 1);

  later.PushResult({1, pupsnes::TickStopReason::kNoWork});
  snes.scheduler->ScheduleDeviceRun(&later, 10);
  snes.scheduler->ScheduleDeviceRun(&later, 30);

  snes.scheduler->Step();
  REQUIRE(later.tick_calls == 1);
  REQUIRE(snes.GetMasterTime() == 30);
}

TEST_CASE(
    "Only one authoritative pending run exists even if stale queue entries "
    "remain",
    "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice device(&snes);

  snes.scheduler->ScheduleDeviceRun(&device, 20);
  snes.scheduler->ScheduleDeviceRun(&device, 10);
  snes.scheduler->ScheduleDeviceRun(&device, 15);

  REQUIRE(pupsnes::SchedulerTestAccess::HasPendingRun(*snes.scheduler, device.GetDeviceId()));
  REQUIRE(pupsnes::SchedulerTestAccess::PendingRunTime(*snes.scheduler, device.GetDeviceId()) == 15);
  REQUIRE(pupsnes::SchedulerTestAccess::QueuedRunEventsFor(*snes.scheduler, device.GetDeviceId()) == 3);
}

TEST_CASE("Scheduler fails loudly on repeated same-time zero-progress runs", "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice device(&snes);

  device.PushResult({0, pupsnes::TickStopReason::kBudgetExhausted});
  device.PushResult({0, pupsnes::TickStopReason::kBudgetExhausted});

  snes.scheduler->ScheduleDeviceRun(&device, 10);

  snes.scheduler->Step();
  REQUIRE(device.tick_calls == 1);

  REQUIRE_THROWS_AS(snes.scheduler->Step(), std::logic_error);
}

TEST_CASE("Stale run event at heap head does not cap budget for the next real event", "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice device(&snes);
  ScriptedDevice other(&snes);

  snes.scheduler->ScheduleDeviceRun(&device, 3);
  snes.scheduler->ScheduleDeviceRun(&device, 8);

  other.PushResult({1, pupsnes::TickStopReason::kBudgetExhausted});
  snes.scheduler->ScheduleDeviceRun(&other, 0);

  snes.scheduler->Step();
  REQUIRE(other.tick_calls == 1);
  REQUIRE(other.budgets.at(0) == 8);
}

TEST_CASE("CatchUpDevice keeps iterating until the target time is reached", "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice device(&snes);

  device.PushResult({2, pupsnes::TickStopReason::kBudgetExhausted});
  device.PushResult({3, pupsnes::TickStopReason::kBudgetExhausted});

  snes.scheduler->CatchUpDevice(device.GetDeviceId(), 5);

  REQUIRE(device.tick_calls == 2);
  REQUIRE(device.start_times == std::vector<pupsnes::TimeMasterT>{0, 2});
  REQUIRE(device.GetTime() == 5);
}

TEST_CASE(
    "CatchUpDevice continues across local boundaries whose wakes are within "
    "the target",
    "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice device(&snes);

  device.PushResult({2, pupsnes::TickStopReason::kReachedLocalBoundary, 0, 4});
  device.PushResult({2, pupsnes::TickStopReason::kBudgetExhausted});

  snes.scheduler->CatchUpDevice(device.GetDeviceId(), 6);

  REQUIRE(device.tick_calls == 2);
  REQUIRE(device.start_times == std::vector<pupsnes::TimeMasterT>{0, 4});
  REQUIRE(device.GetTime() == 6);
}

TEST_CASE("CatchUpDevice rejects invalid synchronous stop reasons", "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice no_work(&snes);
  ScriptedDevice blocked(&snes);

  no_work.PushResult({1, pupsnes::TickStopReason::kNoWork});
  blocked.PushResult({1, pupsnes::TickStopReason::kBlockedOnToken, 7});

  REQUIRE_THROWS_AS(snes.scheduler->CatchUpDevice(no_work.GetDeviceId(), 3), std::logic_error);
  REQUIRE_THROWS_AS(snes.scheduler->CatchUpDevice(blocked.GetDeviceId(), 3), std::logic_error);
}

TEST_CASE("CatchUpDevice rejects a device that is blocked on a token", "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice device(&snes);

  // First run: device blocks on a token.
  device.PushResult({2, pupsnes::TickStopReason::kBlockedOnToken, 99});
  snes.scheduler->ScheduleDeviceRun(&device, 0);
  snes.scheduler->Step();
  REQUIRE(pupsnes::SchedulerTestAccess::BlockedToken(*snes.scheduler, device.GetDeviceId()) == 99);

  // Attempting catch-up while blocked must fail.
  REQUIRE_THROWS_AS(snes.scheduler->CatchUpDevice(device.GetDeviceId(), 10), std::logic_error);
}

TEST_CASE("CatchUpDevice accepts NoWork when the device has reached the target", "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice device(&snes);

  // Device consumes exactly the budget and reports NoWork with a future wake.
  device.PushResult({5, pupsnes::TickStopReason::kNoWork, 0, 20});

  snes.scheduler->CatchUpDevice(device.GetDeviceId(), 5);

  REQUIRE(device.tick_calls == 1);
  REQUIRE(device.GetTime() == 5);
  // HandleRunResult should have scheduled a run at the wake time.
  REQUIRE(pupsnes::SchedulerTestAccess::HasPendingRun(*snes.scheduler, device.GetDeviceId()));
  REQUIRE(pupsnes::SchedulerTestAccess::PendingRunTime(*snes.scheduler, device.GetDeviceId()) == 20);
}

TEST_CASE("CatchUpDevice rejects ReachedLocalBoundary with wake beyond the target", "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice device(&snes);

  // Device stops at a local boundary before the target, but next_wake_time
  // exceeds target.
  device.PushResult({2, pupsnes::TickStopReason::kReachedLocalBoundary, 0, 10});

  REQUIRE_THROWS_AS(snes.scheduler->CatchUpDevice(device.GetDeviceId(), 5), std::logic_error);
}

TEST_CASE(
    "CatchUpDevice with NoWork at target and no wake fully deschedules the "
    "device",
    "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice device(&snes);

  // Device consumes exactly the budget and reports NoWork without a wake time.
  device.PushResult({5, pupsnes::TickStopReason::kNoWork});

  snes.scheduler->CatchUpDevice(device.GetDeviceId(), 5);

  REQUIRE(device.tick_calls == 1);
  REQUIRE(device.GetTime() == 5);
  REQUIRE_FALSE(pupsnes::SchedulerTestAccess::HasPendingRun(*snes.scheduler, device.GetDeviceId()));
}

TEST_CASE(
    "CatchUpDevice with ReachedLocalBoundary exactly at target succeeds and "
    "schedules wake",
    "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice device(&snes);

  // Device reaches target exactly and reports a local boundary with a future
  // wake.
  device.PushResult({5, pupsnes::TickStopReason::kReachedLocalBoundary, 0, 20});

  snes.scheduler->CatchUpDevice(device.GetDeviceId(), 5);

  REQUIRE(device.tick_calls == 1);
  REQUIRE(device.GetTime() == 5);
  REQUIRE(pupsnes::SchedulerTestAccess::HasPendingRun(*snes.scheduler, device.GetDeviceId()));
  REQUIRE(pupsnes::SchedulerTestAccess::PendingRunTime(*snes.scheduler, device.GetDeviceId()) == 20);
}

TEST_CASE("Scheduler CreateToken returns a valid token and schedules CommitComplete", "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice device(&snes);

  const auto token_id =
      snes.scheduler->CreateToken({pupsnes::TokenType::kBusRead, device.GetDeviceId(), 50, 0x2100, 0});

  const auto* token = snes.scheduler->GetToken(token_id);
  REQUIRE(token != nullptr);
  REQUIRE(token->type == pupsnes::TokenType::kBusRead);
  REQUIRE(token->completion_time == 50);

  const auto event = pupsnes::SchedulerTestAccess::PeekNextEvent(*snes.scheduler);
  REQUIRE(event.time == 50);
  REQUIRE(event.subphase == pupsnes::SchedulerPhase::kCommitComplete);
}

TEST_CASE("Scheduler rejects past events", "[unit]") {
  pupsnes::SNES snes;
  ScriptedDevice device(&snes);

  snes.SetMasterTime(100);
  snes.scheduler->ScheduleEvent(50, &device, pupsnes::SchedulerPhase::kWakeSample, pupsnes::EventType::kDeviceBoundary);

  REQUIRE_THROWS_AS(snes.scheduler->Step(), std::logic_error);
}
