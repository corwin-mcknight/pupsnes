#include <catch2/catch_test_macros.hpp>

#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/signal_event.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/sppu/ppu.h"

using namespace pupsnes;

TEST_CASE("MachineSync calls CatchUpTo on every registered device", "[unit][machine_sync]") {
  SNES snes;
  snes.Reset();
  snes.GetPpu().Reset();
  snes.SetMasterTime(5000);
  snes.MachineSync(5000);
  // PPU's CatchUpTo advanced its local_time; stateless-in-time devices keep
  // their default no-op so their time stays at 0.
  REQUIRE(snes.GetPpu().GetTime() == 5000);
}

TEST_CASE("FireEventsThrough fires under a synced machine", "[unit][machine_sync]") {
  SNES snes;
  snes.Reset();
  snes.GetPpu().Reset();
  TimeMasterT ppu_time_at_fire = 0;
  snes.GetScheduler().ScheduleSignal(
      1000, SignalKind::kFrameEnd,
      [&](TimeMasterT /*t*/) { ppu_time_at_fire = snes.GetPpu().GetTime(); });

  snes.SetMasterTime(1000);
  snes.MachineSync(1000);
  snes.GetScheduler().FireEventsThrough(1000);

  REQUIRE(ppu_time_at_fire == 1000);
}
