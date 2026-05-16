#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <utility>
#include <vector>

#include "pupsnes/core/scheduler.h"
#include "pupsnes/core/signal_event.h"
#include "pupsnes/core/snes.h"

using namespace pupsnes;

TEST_CASE("Scheduler::NextEventMasterTime returns max when queue empty", "[unit][scheduler]") {
  // Construct a bare SNES (does not call Reset, so PPU never schedules its
  // first kFrameEnd signal; signal_queue_ is empty at construction).
  SNES snes;
  REQUIRE(snes.GetScheduler().NextEventMasterTime() == std::numeric_limits<TimeMasterT>::max());
}

TEST_CASE("Scheduler fires events in master_time order, drains up to the cutoff", "[unit][scheduler]") {
  SNES snes;
  std::vector<std::pair<TimeMasterT, SignalKind>> fired;
  auto push = [&](TimeMasterT t, SignalKind k) {
    snes.GetScheduler().ScheduleSignal(t, k, [&, t, k](TimeMasterT fired_at) {
      REQUIRE(fired_at == t);
      fired.emplace_back(t, k);
    });
  };
  push(200, SignalKind::kFrameEnd);
  push(100, SignalKind::kHIrqMatch);
  push(150, SignalKind::kVblankNmiBoundary);

  REQUIRE(snes.GetScheduler().NextEventMasterTime() == 100);
  snes.GetScheduler().FireEventsThrough(180);
  REQUIRE(fired.size() == 2);
  REQUIRE(fired[0].first == 100);
  REQUIRE(fired[1].first == 150);
  REQUIRE(snes.GetScheduler().NextEventMasterTime() == 200);
  snes.GetScheduler().FireEventsThrough(10000);
  REQUIRE(fired.size() == 3);
}

TEST_CASE("Scheduler snapshot is a stable read-only view", "[unit][scheduler]") {
  SNES snes;
  snes.GetScheduler().ScheduleSignal(500, SignalKind::kFrameEnd, {});
  snes.GetScheduler().ScheduleSignal(100, SignalKind::kVblankNmiBoundary, {});
  const auto snapshot = snes.GetScheduler().SnapshotSignalQueue();
  REQUIRE(snapshot.size() == 2);
  REQUIRE(snapshot[0].master_time == 100);
  REQUIRE(snapshot[0].kind == SignalKind::kVblankNmiBoundary);
  REQUIRE(snapshot[1].master_time == 500);
}
