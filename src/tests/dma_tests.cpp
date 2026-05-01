#include <catch2/catch_test_macros.hpp>

#include "pupsnes/hw/dma_controller.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"

using namespace pupsnes;  // NOLINT

namespace {

// These helpers are referenced by tests landing in subsequent DMA tasks
// (writes through the bus, reads of $43xx shadows). Marked maybe_unused so
// -Wunused-function doesn't fail the build while only the skeleton test exists.
[[maybe_unused]] void BusWrite(SNES& snes, SnesAddrT address, uint8_t data, TimeMasterT now) {
  BusPlan plan = snes.system_bus->Plan(address, BusAccessType::kWrite, data);
  REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
  (void)snes.system_bus->Follow(plan, now, 0);
}

[[maybe_unused, nodiscard]] uint8_t BusRead(SNES& snes, SnesAddrT address, TimeMasterT now) {
  BusPlan plan = snes.system_bus->Plan(address, BusAccessType::kRead);
  REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
  return snes.system_bus->Follow(plan, now, 0).data;
}

}  // namespace

TEST_CASE("DMA controller exists and resets to zero state", "[unit][dma]") {
  SNES snes;
  DmaController& dma = snes.GetDma();
  REQUIRE(dma.GetChannelState(0).dmap == 0);
  REQUIRE(dma.GetChannelState(7).dmap == 0);
  REQUIRE(dma.GetChannelState(0).bbad == 0);
  REQUIRE(dma.GetChannelState(0).a1t == 0);
  REQUIRE(dma.GetChannelState(0).a1b == 0);
  REQUIRE(dma.GetChannelState(0).das == 0);
}
