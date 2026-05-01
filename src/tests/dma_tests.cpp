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

TEST_CASE("DMA channel 0 register writes update channel state", "[unit][dma]") {
  SNES snes;
  DmaController& dma = snes.GetDma();
  TimeMasterT now = 1;

  // $4300 DMAP0, $4301 BBAD0, $4302/3 A1T0, $4304 A1B0, $4305/6 DAS0, $4307 DASB0.
  BusWrite(snes, 0x4300U, 0x12U, now++);
  BusWrite(snes, 0x4301U, 0x18U, now++);  // VRAM data port
  BusWrite(snes, 0x4302U, 0x34U, now++);
  BusWrite(snes, 0x4303U, 0x12U, now++);  // a1t = 0x1234
  BusWrite(snes, 0x4304U, 0x80U, now++);
  BusWrite(snes, 0x4305U, 0x00U, now++);
  BusWrite(snes, 0x4306U, 0x10U, now++);  // das = 0x1000
  BusWrite(snes, 0x4307U, 0xABU, now++);

  const auto& ch = dma.GetChannelState(0);
  REQUIRE(ch.dmap == 0x12U);
  REQUIRE(ch.bbad == 0x18U);
  REQUIRE(ch.a1t == 0x1234U);
  REQUIRE(ch.a1b == 0x80U);
  REQUIRE(ch.das == 0x1000U);
  REQUIRE(ch.dasb == 0xABU);
}

TEST_CASE("DMA channel 7 register writes target the correct channel", "[unit][dma]") {
  SNES snes;
  DmaController& dma = snes.GetDma();
  TimeMasterT now = 1;

  BusWrite(snes, 0x4370U, 0xAAU, now++);
  REQUIRE(dma.GetChannelState(7).dmap == 0xAAU);
  REQUIRE(dma.GetChannelState(0).dmap == 0x00U);
}

TEST_CASE("DMA channel registers read back what was written", "[unit][dma]") {
  SNES snes;
  TimeMasterT now = 1;

  BusWrite(snes, 0x4300U, 0x12U, now++);
  BusWrite(snes, 0x4301U, 0x18U, now++);
  BusWrite(snes, 0x4302U, 0x34U, now++);
  BusWrite(snes, 0x4303U, 0x12U, now++);
  BusWrite(snes, 0x4304U, 0x80U, now++);
  BusWrite(snes, 0x4305U, 0x00U, now++);
  BusWrite(snes, 0x4306U, 0x10U, now++);
  BusWrite(snes, 0x4307U, 0xABU, now++);

  REQUIRE(BusRead(snes, 0x4300U, now++) == 0x12U);
  REQUIRE(BusRead(snes, 0x4301U, now++) == 0x18U);
  REQUIRE(BusRead(snes, 0x4302U, now++) == 0x34U);
  REQUIRE(BusRead(snes, 0x4303U, now++) == 0x12U);
  REQUIRE(BusRead(snes, 0x4304U, now++) == 0x80U);
  REQUIRE(BusRead(snes, 0x4305U, now++) == 0x00U);
  REQUIRE(BusRead(snes, 0x4306U, now++) == 0x10U);
  REQUIRE(BusRead(snes, 0x4307U, now++) == 0xABU);
}

TEST_CASE("$420B MDMAEN write delegates to DmaController::Trigger", "[unit][dma]") {
  SNES snes;
  DmaController& dma = snes.GetDma();
  TimeMasterT now = 100;

  // Configure channel 0 with a tiny transfer (1 byte, mode 0, A->B, increment).
  // Source: $00:1000 (cartridge ROM space - value doesn't matter for this test).
  BusWrite(snes, 0x4300U, 0x00U, now++);  // DMAP: A->B, increment, mode 0
  BusWrite(snes, 0x4301U, 0x18U, now++);  // BBAD = $18 ($2118 VRAM)
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x10U, now++);  // a1t = $1000
  BusWrite(snes, 0x4304U, 0x00U, now++);  // a1b = $00
  BusWrite(snes, 0x4305U, 0x01U, now++);
  BusWrite(snes, 0x4306U, 0x00U, now++);  // das = 1 byte

  // Triggering channel 0 should clear DAS to 0.
  BusWrite(snes, 0x420BU, 0x01U, now++);
  REQUIRE(dma.GetChannelState(0).das == 0U);
}
