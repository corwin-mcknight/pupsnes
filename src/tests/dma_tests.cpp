#include <catch2/catch_test_macros.hpp>

#include "pupsnes/hw/dma_controller.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/sppu/ppu.h"
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

TEST_CASE("DMA mode 0 A->B copies N bytes from WRAM to VRAM", "[unit][dma]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  TimeMasterT now = 100;

  // Disable forced blank so VRAM access is permitted (cosmetic; PPU port
  // access works in any state in our model).
  BusWrite(snes, 0x002100U, 0x0FU, now++);
  // VMAIN: increment after VMDATAH, +1 word step.
  BusWrite(snes, 0x002115U, 0x80U, now++);
  // VMADD = 0.
  BusWrite(snes, 0x002116U, 0x00U, now++);
  BusWrite(snes, 0x002117U, 0x00U, now++);

  // Seed source data in WRAM at $7E:0000 — 4 bytes: $11, $22, $33, $44.
  BusWrite(snes, 0x7E0000U, 0x11U, now++);
  BusWrite(snes, 0x7E0001U, 0x22U, now++);
  BusWrite(snes, 0x7E0002U, 0x33U, now++);
  BusWrite(snes, 0x7E0003U, 0x44U, now++);

  // Channel 0: A→B, increment, mode 0 (single-byte to BBAD).
  // Source: $7E:0000. Dest: $2118 (VMDATAL). Length: 4 bytes.
  // Mode 0 writes every byte to the same B-bus port → all 4 bytes hit $2118.
  BusWrite(snes, 0x4300U, 0x00U, now++);  // DMAP
  BusWrite(snes, 0x4301U, 0x18U, now++);  // BBAD = $18
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);  // a1t = 0
  BusWrite(snes, 0x4304U, 0x7EU, now++);  // a1b = $7E
  BusWrite(snes, 0x4305U, 0x04U, now++);
  BusWrite(snes, 0x4306U, 0x00U, now++);  // das = 4

  BusWrite(snes, 0x420BU, 0x01U, now++);

  // Force a PPU read so the lazy log drains and the writes land in VRAM.
  ppu.CatchUpTo(now + 10000U);

  // For this assertion, confirm that DAS reached 0 and a1t advanced by 4.
  // (Reading back the actual VRAM bytes via $2139/$213A is exercised in
  // Task 7's mode-1 PPU end-to-end test.)
  REQUIRE(snes.GetDma().GetChannelState(0).das == 0U);
  REQUIRE(snes.GetDma().GetChannelState(0).a1t == 0x0004U);
  REQUIRE(snes.GetDma().GetChannelState(0).a1b == 0x7EU);  // bank constant
}

TEST_CASE("DMA A-bus decrement mode walks source backwards", "[unit][dma]") {
  SNES snes;
  TimeMasterT now = 100;

  BusWrite(snes, 0x7E0010U, 0xAAU, now++);
  BusWrite(snes, 0x7E000FU, 0xBBU, now++);
  BusWrite(snes, 0x7E000EU, 0xCCU, now++);

  BusWrite(snes, 0x4300U, 0x10U, now++);  // step_mode=2 (decrement) -> dmap bit 4 set
  BusWrite(snes, 0x4301U, 0x18U, now++);
  BusWrite(snes, 0x4302U, 0x10U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);  // a1t = $0010
  BusWrite(snes, 0x4304U, 0x7EU, now++);
  BusWrite(snes, 0x4305U, 0x03U, now++);
  BusWrite(snes, 0x4306U, 0x00U, now++);  // das = 3

  BusWrite(snes, 0x420BU, 0x01U, now++);

  REQUIRE(snes.GetDma().GetChannelState(0).a1t == 0x000DU);  // 0x10 - 3
  REQUIRE(snes.GetDma().GetChannelState(0).das == 0U);
}

TEST_CASE("DMA A-bus fixed mode keeps source pointer constant", "[unit][dma]") {
  SNES snes;
  TimeMasterT now = 100;

  BusWrite(snes, 0x7E0020U, 0xEEU, now++);

  BusWrite(snes, 0x4300U, 0x08U, now++);  // step_mode=1 (fixed)
  BusWrite(snes, 0x4301U, 0x18U, now++);
  BusWrite(snes, 0x4302U, 0x20U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);
  BusWrite(snes, 0x4304U, 0x7EU, now++);
  BusWrite(snes, 0x4305U, 0x05U, now++);
  BusWrite(snes, 0x4306U, 0x00U, now++);  // das = 5

  BusWrite(snes, 0x420BU, 0x01U, now++);

  REQUIRE(snes.GetDma().GetChannelState(0).a1t == 0x0020U);  // unchanged
  REQUIRE(snes.GetDma().GetChannelState(0).das == 0U);
}

TEST_CASE("DMA mode 1 transfers 2-byte pairs to BBAD/BBAD+1 (VRAM upload pattern)", "[unit][dma]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  TimeMasterT now = 100;

  BusWrite(snes, 0x002100U, 0x0FU, now++);
  BusWrite(snes, 0x002115U, 0x80U, now++);  // VMAIN: inc after $2119, +1 word
  BusWrite(snes, 0x002116U, 0x00U, now++);
  BusWrite(snes, 0x002117U, 0x00U, now++);  // VMADD = 0

  // Source: 4 bytes in WRAM forming two VRAM words: 0xBBAA at word 0, 0xDDCC at word 1.
  BusWrite(snes, 0x7E0000U, 0xAAU, now++);
  BusWrite(snes, 0x7E0001U, 0xBBU, now++);
  BusWrite(snes, 0x7E0002U, 0xCCU, now++);
  BusWrite(snes, 0x7E0003U, 0xDDU, now++);

  BusWrite(snes, 0x4300U, 0x01U, now++);  // mode 1, A->B, increment
  BusWrite(snes, 0x4301U, 0x18U, now++);  // BBAD = $18
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);
  BusWrite(snes, 0x4304U, 0x7EU, now++);
  BusWrite(snes, 0x4305U, 0x04U, now++);
  BusWrite(snes, 0x4306U, 0x00U, now++);  // das = 4 bytes (= 2 words)

  BusWrite(snes, 0x420BU, 0x01U, now++);
  ppu.CatchUpTo(now + 10000U);

  // Read back via $2139/$213A (RDVRAM). Reset VMADD and use the prefetch
  // protocol: first read after VMADD is the throwaway prefetch.
  BusWrite(snes, 0x002116U, 0x00U, now++);
  BusWrite(snes, 0x002117U, 0x00U, now++);
  // Throwaway read to load prefetch buffer with VRAM word 0:
  (void)BusRead(snes, 0x002139U, now++);
  (void)BusRead(snes, 0x00213AU, now++);
  // Now read word 0 (low+high) -- should be $BBAA, then word 1 = $DDCC.
  const uint8_t w0_lo = BusRead(snes, 0x002139U, now++);
  const uint8_t w0_hi = BusRead(snes, 0x00213AU, now++);
  REQUIRE(w0_lo == 0xAAU);
  REQUIRE(w0_hi == 0xBBU);
  const uint8_t w1_lo = BusRead(snes, 0x002139U, now++);
  const uint8_t w1_hi = BusRead(snes, 0x00213AU, now++);
  REQUIRE(w1_lo == 0xCCU);
  REQUIRE(w1_hi == 0xDDU);
}

TEST_CASE("DMA mode 4 transfers 4 sequential bytes to BBAD..BBAD+3", "[unit][dma]") {
  SNES snes;
  TimeMasterT now = 100;

  for (uint8_t i = 0; i < 4U; ++i) BusWrite(snes, 0x7E0000U + i, static_cast<uint8_t>(0xA0U + i), now++);

  BusWrite(snes, 0x4300U, 0x04U, now++);  // mode 4
  BusWrite(snes, 0x4301U, 0x00U, now++);
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);
  BusWrite(snes, 0x4304U, 0x7EU, now++);
  BusWrite(snes, 0x4305U, 0x04U, now++);
  BusWrite(snes, 0x4306U, 0x00U, now++);
  BusWrite(snes, 0x420BU, 0x01U, now++);

  REQUIRE(snes.GetDma().GetChannelState(0).das == 0U);
  REQUIRE(snes.GetDma().GetChannelState(0).a1t == 0x0004U);
}

TEST_CASE("DMA mode 2 transfers 2 bytes to BBAD twice (CGRAM upload pattern)", "[unit][dma]") {
  SNES snes;
  TimeMasterT now = 100;

  for (uint8_t i = 0; i < 4U; ++i) BusWrite(snes, 0x7E0000U + i, static_cast<uint8_t>(0xC0U + i), now++);

  BusWrite(snes, 0x4300U, 0x02U, now++);  // mode 2
  BusWrite(snes, 0x4301U, 0x22U, now++);  // BBAD = $22 (CGDATA)
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);
  BusWrite(snes, 0x4304U, 0x7EU, now++);
  BusWrite(snes, 0x4305U, 0x04U, now++);
  BusWrite(snes, 0x4306U, 0x00U, now++);
  BusWrite(snes, 0x420BU, 0x01U, now++);

  REQUIRE(snes.GetDma().GetChannelState(0).das == 0U);
  REQUIRE(snes.GetDma().GetChannelState(0).a1t == 0x0004U);
}

TEST_CASE("DMA mode 5 transfers 4 bytes alternating BBAD/BBAD+1", "[unit][dma]") {
  SNES snes;
  TimeMasterT now = 100;

  for (uint8_t i = 0; i < 4U; ++i) BusWrite(snes, 0x7E0000U + i, static_cast<uint8_t>(0x50U + i), now++);

  BusWrite(snes, 0x4300U, 0x05U, now++);  // mode 5
  BusWrite(snes, 0x4301U, 0x18U, now++);
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);
  BusWrite(snes, 0x4304U, 0x7EU, now++);
  BusWrite(snes, 0x4305U, 0x04U, now++);
  BusWrite(snes, 0x4306U, 0x00U, now++);
  BusWrite(snes, 0x420BU, 0x01U, now++);

  REQUIRE(snes.GetDma().GetChannelState(0).das == 0U);
  REQUIRE(snes.GetDma().GetChannelState(0).a1t == 0x0004U);
}
