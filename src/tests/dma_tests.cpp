#include <catch2/catch_test_macros.hpp>
#include <limits>

#include "pupsnes/hw/5a22/cpu_mmio.h"
#include "pupsnes/hw/5a22/dma_controller.h"
#include "pupsnes/core/scheduler.h"
#include "pupsnes/core/signal_event.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/memory/systembus.h"

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

TEST_CASE("DMA $420B with two channels runs channel 0 then channel 1", "[unit][dma]") {
  SNES snes;
  TimeMasterT now = 100;

  // Two transfers writing to a B-bus port we can observe — use OAM ($2104)
  // since OAM bytes are independently addressable and persistent. Source A: a
  // single byte. Source B: a single byte. Verify both channels' state cleared.

  BusWrite(snes, 0x7E0000U, 0x11U, now++);
  BusWrite(snes, 0x7E0010U, 0x22U, now++);

  // Channel 0
  BusWrite(snes, 0x4300U, 0x00U, now++);
  BusWrite(snes, 0x4301U, 0x04U, now++);  // BBAD = $04 ($2104 OAM)
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);
  BusWrite(snes, 0x4304U, 0x7EU, now++);
  BusWrite(snes, 0x4305U, 0x01U, now++);
  BusWrite(snes, 0x4306U, 0x00U, now++);

  // Channel 1
  BusWrite(snes, 0x4310U, 0x00U, now++);
  BusWrite(snes, 0x4311U, 0x04U, now++);
  BusWrite(snes, 0x4312U, 0x10U, now++);
  BusWrite(snes, 0x4313U, 0x00U, now++);
  BusWrite(snes, 0x4314U, 0x7EU, now++);
  BusWrite(snes, 0x4315U, 0x01U, now++);
  BusWrite(snes, 0x4316U, 0x00U, now++);

  // Trigger both.
  BusWrite(snes, 0x420BU, 0x03U, now++);

  REQUIRE(snes.GetDma().GetChannelState(0).das == 0U);
  REQUIRE(snes.GetDma().GetChannelState(1).das == 0U);
  REQUIRE(snes.GetDma().GetChannelState(0).a1t == 0x0001U);
  REQUIRE(snes.GetDma().GetChannelState(1).a1t == 0x0011U);
}

TEST_CASE("DMA stalls the CPU by 8 master cycles per byte + startup", "[unit][dma]") {
  SNES snes;
  TimeMasterT now = 1000;

  // 16-byte transfer: expected cost = 8 (startup) + 16 * 8 = 136 master cycles.
  BusWrite(snes, 0x4300U, 0x00U, now++);
  BusWrite(snes, 0x4301U, 0x18U, now++);
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);
  BusWrite(snes, 0x4304U, 0x7EU, now++);
  BusWrite(snes, 0x4305U, 0x10U, now++);
  BusWrite(snes, 0x4306U, 0x00U, now++);

  const TimeMasterT before = snes.GetMasterTime();
  // Trigger directly via the DMA controller to isolate the cost from the
  // surrounding bus access overhead.
  const TimeMasterT after = snes.GetDma().Trigger(0x01U, before);
  REQUIRE(after - before == 8U + 16U * 8U);
}

TEST_CASE("$420B write advances master_time by full DMA cost", "[unit][dma]") {
  SNES snes;
  TimeMasterT now = 1000;
  snes.SetMasterTime(now);

  BusWrite(snes, 0x4300U, 0x00U, now++);
  BusWrite(snes, 0x4301U, 0x18U, now++);
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);
  BusWrite(snes, 0x4304U, 0x7EU, now++);
  BusWrite(snes, 0x4305U, 0x04U, now++);
  BusWrite(snes, 0x4306U, 0x00U, now++);

  const TimeMasterT before_trigger = now;
  BusWrite(snes, 0x420BU, 0x01U, now);  // Note: not now++ — we want the timestamp of the trigger.
  REQUIRE(snes.GetMasterTime() >= before_trigger + 8U + 4U * 8U);
}

TEST_CASE("DMA upload of palette + tilemap + char data renders a Mode 1 tile", "[integration][dma][ppu]") {
  SNES snes;
  Ppu& ppu = snes.GetPpu();
  TimeMasterT now = 100;

  BusWrite(snes, 0x002100U, 0x0FU, now++);  // brightness 15, no forced blank
  BusWrite(snes, 0x002115U, 0x80U, now++);  // VMAIN: inc after $2119

  // --- Build source data in WRAM ---
  // 16 words = one 4bpp 8x8 tile, all color index 1.
  // Plane 0 row N: byte 0xFF (all columns set in plane 0).
  // Planes 1/2/3: 0.
  for (uint16_t row = 0; row < 8; ++row) {
    BusWrite(snes, 0x7E0000U + row * 2U, 0xFFU, now++);
    BusWrite(snes, 0x7E0001U + row * 2U, 0x00U, now++);
  }
  for (uint16_t row = 0; row < 8; ++row) {
    BusWrite(snes, 0x7E0010U + row * 2U, 0x00U, now++);
    BusWrite(snes, 0x7E0011U + row * 2U, 0x00U, now++);
  }
  // Tilemap entry at WRAM $7E:0100 = 0x0000 (char 0, palette 0, no flip, no priority).
  BusWrite(snes, 0x7E0100U, 0x00U, now++);
  BusWrite(snes, 0x7E0101U, 0x00U, now++);
  // CGRAM bytes at WRAM $7E:0200: backdrop (0,0), color 1 = 0x7FFF (white).
  BusWrite(snes, 0x7E0200U, 0x00U, now++);
  BusWrite(snes, 0x7E0201U, 0x00U, now++);
  BusWrite(snes, 0x7E0202U, 0xFFU, now++);
  BusWrite(snes, 0x7E0203U, 0x7FU, now++);

  // --- DMA channel 0: WRAM $7E:0000..$7E:001F → VRAM word 0x1000 (= byte $2000) ---
  // Set VMADD first.
  BusWrite(snes, 0x002116U, 0x00U, now++);
  BusWrite(snes, 0x002117U, 0x10U, now++);  // VMADD = 0x1000

  BusWrite(snes, 0x4300U, 0x01U, now++);  // mode 1, A→B, increment
  BusWrite(snes, 0x4301U, 0x18U, now++);  // BBAD = $18 (VMDATAL)
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);
  BusWrite(snes, 0x4304U, 0x7EU, now++);
  BusWrite(snes, 0x4305U, 0x20U, now++);
  BusWrite(snes, 0x4306U, 0x00U, now++);  // das = 32 bytes
  BusWrite(snes, 0x420BU, 0x01U, now++);

  // --- DMA channel 0 reused: tilemap entry → VRAM word 0 ---
  BusWrite(snes, 0x002116U, 0x00U, now++);
  BusWrite(snes, 0x002117U, 0x00U, now++);
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x01U, now++);  // a1t = 0x0100
  BusWrite(snes, 0x4305U, 0x02U, now++);
  BusWrite(snes, 0x4306U, 0x00U, now++);
  BusWrite(snes, 0x420BU, 0x01U, now++);

  // --- DMA channel 0: CGRAM bytes → CGRAM via $2122 ---
  BusWrite(snes, 0x002121U, 0x00U, now++);  // CGADD = 0
  BusWrite(snes, 0x4300U, 0x00U, now++);    // mode 0 (every byte to BBAD)
  BusWrite(snes, 0x4301U, 0x22U, now++);    // BBAD = $22 (CGDATA)
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x02U, now++);  // a1t = 0x0200
  BusWrite(snes, 0x4305U, 0x04U, now++);
  BusWrite(snes, 0x4306U, 0x00U, now++);
  BusWrite(snes, 0x420BU, 0x01U, now++);

  // --- Configure Mode 1 BG1 to draw the tile at (0,0) ---
  BusWrite(snes, 0x002105U, 0x01U, now++);  // BGMODE = 1
  BusWrite(snes, 0x002107U, 0x00U, now++);  // BG1SC: tilemap base 0
  BusWrite(snes, 0x00210BU, 0x01U, now++);  // BG12NBA: BG1 char base nibble = 1
  BusWrite(snes, 0x00210DU, 0x00U, now++);
  BusWrite(snes, 0x00210DU, 0x00U, now++);  // BG1 H scroll = 0
  BusWrite(snes, 0x00210EU, 0x00U, now++);
  BusWrite(snes, 0x00210EU, 0x00U, now++);  // BG1 V scroll = 0
  BusWrite(snes, 0x00212CU, 0x01U, now++);  // TM = BG1

  // Render a frame.
  ppu.CatchUpTo(262U * 1364U + 100U);
  const FrameBufferView view = ppu.BuildFrontView();

  REQUIRE(view.pixels[0U] == 0x7FFFU);  // white at (0,0)
  REQUIRE(view.pixels[7U] == 0x7FFFU);  // white at (7,0)
}

// -----------------------------------------------------------------------------
// HDMA tests start here. HDMA fires through the scheduler at master-time-fixed
// points within each frame (V=0 H=6 init; V=0..224 H=274 per-line transfer).
// -----------------------------------------------------------------------------

namespace {

// Count scheduled signal events of a given kind whose master_time is <= deadline.
[[maybe_unused]] std::size_t CountSignalsAt(const SNES& snes, SignalKind kind, TimeMasterT deadline) {
  std::size_t count = 0;
  for (const auto& view : snes.scheduler->SnapshotSignalQueue()) {
    if (view.kind == kind && view.master_time <= deadline) {
      ++count;
    }
  }
  return count;
}

// Find the earliest scheduled event of `kind`. Returns std::numeric_limits-style
// max sentinel when no such event is pending.
[[maybe_unused, nodiscard]] TimeMasterT EarliestSignal(const SNES& snes, SignalKind kind) {
  TimeMasterT earliest = std::numeric_limits<TimeMasterT>::max();
  for (const auto& view : snes.scheduler->SnapshotSignalQueue()) {
    if (view.kind == kind && view.master_time < earliest) {
      earliest = view.master_time;
    }
  }
  return earliest;
}

// Advance master_time to `target` while properly interleaving MachineSync and
// scheduler event firing. Mirrors what RunControl does at the top level — for
// each scheduled event whose time <= target, sync every device to that event
// time, then fire the event. After all such events have fired, sync once more
// to `target` so devices end up caught up to the requested deadline. Critical
// for any test where event handlers enqueue lazy-replay writes: a single
// MachineSync-then-FireEventsThrough pair leaves the writes orphaned because
// the PPU is already past the write cycle.
[[maybe_unused]] void RunToTime(SNES& snes, TimeMasterT target) {
  while (true) {
    const TimeMasterT next = snes.GetScheduler().NextEventMasterTime();
    if (next > target) break;
    snes.MachineSync(next);
    snes.GetScheduler().FireEventsThrough(next);
  }
  snes.MachineSync(target);
}

}  // namespace

TEST_CASE("HDMA schedules its first init signal at V=0 H=6 after Reset", "[unit][dma][hdma]") {
  SNES snes;
  snes.Reset();

  // V=0 H=6 in master cycles: each dot before H=323 is 4 mcyc, so H=6 = 24 mcyc.
  const TimeMasterT expected = 24U;
  REQUIRE(EarliestSignal(snes, SignalKind::kHdmaFire) == expected);
}

TEST_CASE("HDMA init copies A1T to A2A and snapshots HDMAEN", "[unit][dma][hdma]") {
  SNES snes;
  snes.Reset();
  TimeMasterT now = 1;

  // Configure channel 0 with a known source-table address.
  BusWrite(snes, 0x4300U, 0x00U, now++);  // DMAP: direct, mode 0
  BusWrite(snes, 0x4301U, 0x0DU, now++);  // BBAD: $210D (BG1HOFS)
  BusWrite(snes, 0x4302U, 0x34U, now++);
  BusWrite(snes, 0x4303U, 0x12U, now++);  // A1T = 0x1234
  BusWrite(snes, 0x4304U, 0x7EU, now++);  // A1B = $7E (WRAM)

  // Enable HDMA on channel 0 (HDMAEN bit 0).
  BusWrite(snes, 0x420CU, 0x01U, now++);

  // Fire the init signal. MachineSync first so devices catch up, then fire
  // events through master_time=24.
  snes.MachineSync(30U);
  snes.GetScheduler().FireEventsThrough(30U);

  const auto& ch = snes.GetDma().GetChannelState(0);
  REQUIRE(ch.a2a == 0x34U);
  REQUIRE(ch.a2a_high == 0x12U);
  REQUIRE(ch.ntrl == 0x00U);
  REQUIRE(snes.GetDma().GetHdmaActiveMask() == 0x01U);
}

TEST_CASE("HDMA direct mode advances A2A and decrements NTRL per line", "[unit][dma][hdma]") {
  SNES snes;
  snes.Reset();
  TimeMasterT now = 1;

  // Source table at $7E:0000 — count=1 / byte / count=1 / byte / terminator.
  // Two single-line entries means: line 0 transfers the first byte, line 1
  // transfers the second, line 2 hits the 0 terminator and the channel finishes.
  BusWrite(snes, 0x7E0000U, 0x01U, now++);
  BusWrite(snes, 0x7E0001U, 0xAAU, now++);
  BusWrite(snes, 0x7E0002U, 0x01U, now++);
  BusWrite(snes, 0x7E0003U, 0xBBU, now++);
  BusWrite(snes, 0x7E0004U, 0x00U, now++);

  BusWrite(snes, 0x4300U, 0x00U, now++);  // DMAP: direct, mode 0
  BusWrite(snes, 0x4301U, 0x00U, now++);  // BBAD: $2100 (INIDISP — observable via PPU)
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);  // A1T = 0x0000
  BusWrite(snes, 0x4304U, 0x7EU, now++);  // A1B = $7E
  BusWrite(snes, 0x420CU, 0x01U, now++);

  // Fire init only (master_time = 24).
  snes.MachineSync(30U);
  snes.GetScheduler().FireEventsThrough(30U);
  REQUIRE(snes.GetDma().GetChannelState(0).a2a == 0x00U);
  REQUIRE(snes.GetDma().GetChannelState(0).ntrl == 0x00U);

  // Fire V=0 H=274 (master_time = 1096). Should load header (1), transfer 0xAA
  // from $7E:0001 to $2100, advance A2A to 2, decrement NTRL to 0.
  const TimeMasterT v0_line = 1096U;
  snes.MachineSync(v0_line + 16U);
  snes.GetScheduler().FireEventsThrough(v0_line + 16U);
  REQUIRE(snes.GetDma().GetChannelState(0).a2a == 0x02U);
  REQUIRE(snes.GetDma().GetChannelState(0).ntrl == 0x00U);

  // Fire V=1 H=274 (master_time = 1096 + 1364 = 2460). Reloads header (1),
  // transfers 0xBB from $7E:0003, advances A2A to 4.
  const TimeMasterT v1_line = v0_line + 1364U;
  snes.MachineSync(v1_line + 16U);
  snes.GetScheduler().FireEventsThrough(v1_line + 16U);
  REQUIRE(snes.GetDma().GetChannelState(0).a2a == 0x04U);
  REQUIRE(snes.GetDma().GetChannelState(0).ntrl == 0x00U);

  // Fire V=2 H=274. Header byte at $7E:0004 = 0x00 → channel finished.
  const TimeMasterT v2_line = v1_line + 1364U;
  snes.MachineSync(v2_line + 16U);
  snes.GetScheduler().FireEventsThrough(v2_line + 16U);
  REQUIRE(snes.GetDma().GetChannelState(0).hdma_finished == true);
}

TEST_CASE("HDMA direct mode writes reach the destination port (INIDISP brightness)", "[unit][dma][hdma]") {
  SNES snes;
  snes.Reset();
  Ppu& ppu = snes.GetPpu();
  TimeMasterT now = 1;

  // Source: {0x01, 0x07, 0x01, 0x0F, 0x00} — line 0 sets INIDISP=$07
  // (brightness 7), line 1 sets INIDISP=$0F (brightness 15, force-blank cleared).
  BusWrite(snes, 0x7E0000U, 0x01U, now++);
  BusWrite(snes, 0x7E0001U, 0x07U, now++);
  BusWrite(snes, 0x7E0002U, 0x01U, now++);
  BusWrite(snes, 0x7E0003U, 0x0FU, now++);
  BusWrite(snes, 0x7E0004U, 0x00U, now++);

  BusWrite(snes, 0x4300U, 0x00U, now++);
  BusWrite(snes, 0x4301U, 0x00U, now++);  // BBAD = $00 → $2100 INIDISP
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);
  BusWrite(snes, 0x4304U, 0x7EU, now++);
  BusWrite(snes, 0x420CU, 0x01U, now++);

  // Drive through V=1 to let two lines of HDMA fire. The MachineSync below
  // advances the PPU to 3000 first, then events fire and enqueue lazy-replay
  // writes at cycles ~1108 and ~2472. A subsequent ppu.CatchUpTo past 3000
  // runs one more dot and drains those queued writes.
  snes.MachineSync(3000U);
  snes.GetScheduler().FireEventsThrough(3000U);
  ppu.CatchUpTo(3050U);

  REQUIRE(ppu.GetBrightness() == 0x0FU);
  REQUIRE(ppu.IsForcedBlank() == false);  // bit 7 of $0F is 0
}

TEST_CASE("HDMA indirect mode loads DAS from source table and transfers from DASB:DAS", "[unit][dma][hdma]") {
  SNES snes;
  snes.Reset();
  Ppu& ppu = snes.GetPpu();
  TimeMasterT now = 1;

  // Source table at $7E:0000: count=1, indirect_lo=0x10, indirect_hi=0x00, terminator.
  // Indirect data at $7E:0010: a single byte 0x0A → INIDISP brightness 0x0A.
  BusWrite(snes, 0x7E0000U, 0x01U, now++);
  BusWrite(snes, 0x7E0001U, 0x10U, now++);
  BusWrite(snes, 0x7E0002U, 0x00U, now++);
  BusWrite(snes, 0x7E0003U, 0x00U, now++);  // terminator (after 1-line entry)
  BusWrite(snes, 0x7E0010U, 0x0AU, now++);

  BusWrite(snes, 0x4300U, 0x40U, now++);  // DMAP: indirect, mode 0
  BusWrite(snes, 0x4301U, 0x00U, now++);  // BBAD: $2100 INIDISP
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);  // A1T = 0x0000
  BusWrite(snes, 0x4304U, 0x7EU, now++);  // A1B = $7E
  BusWrite(snes, 0x4307U, 0x7EU, now++);  // DASB = $7E (indirect data bank)
  BusWrite(snes, 0x420CU, 0x01U, now++);

  snes.MachineSync(2000U);
  snes.GetScheduler().FireEventsThrough(2000U);
  ppu.CatchUpTo(2050U);

  // After init: a2a=0, ntrl=0.
  // After V=0 line: header reload (a2a 0→1, ntrl=1), indirect DAS load
  //   (read 2 bytes from $7E:0001-0002, a2a 1→3, DAS=0x0010), then one byte
  //   transferred from $7E:0010 = 0x0A to $2100. DAS post-increments to
  //   0x0011. NTRL decrements to 0; do_transfer = true (sets up reload next line).
  REQUIRE(snes.GetDma().GetChannelState(0).a2a == 0x03U);
  REQUIRE(snes.GetDma().GetChannelState(0).das == 0x0011U);
  REQUIRE(ppu.GetBrightness() == 0x0AU);
}

TEST_CASE("HDMA integration: INIDISP brightness modulated per scanline produces banded output",
          "[integration][dma][hdma]") {
  SNES snes;
  snes.Reset();
  Ppu& ppu = snes.GetPpu();
  TimeMasterT now = 1;

  // Make the backdrop (CGRAM word 0) white so brightness scaling is observable
  // on every pixel: rendered pixel = white × (brightness+1)/16.
  BusWrite(snes, 0x002121U, 0x00U, now++);  // CGADD = 0
  BusWrite(snes, 0x002122U, 0xFFU, now++);
  BusWrite(snes, 0x002122U, 0x7FU, now++);  // word 0 = 0x7FFF

  // HDMA source table: three 8-line entries setting INIDISP to $0F, $08, $04.
  // Each entry covers 8 scanlines, then INIDISP persists at the last value
  // until the next reload (which here is the terminator).
  BusWrite(snes, 0x7E0000U, 0x08U, now++);
  BusWrite(snes, 0x7E0001U, 0x0FU, now++);  // brightness 15 for V=0..7
  BusWrite(snes, 0x7E0002U, 0x08U, now++);
  BusWrite(snes, 0x7E0003U, 0x08U, now++);  // brightness 8 for V=8..15
  BusWrite(snes, 0x7E0004U, 0x08U, now++);
  BusWrite(snes, 0x7E0005U, 0x04U, now++);  // brightness 4 for V=16..23
  BusWrite(snes, 0x7E0006U, 0x00U, now++);  // terminator

  BusWrite(snes, 0x4300U, 0x00U, now++);  // direct, mode 0
  BusWrite(snes, 0x4301U, 0x00U, now++);  // BBAD = $00 (INIDISP)
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);
  BusWrite(snes, 0x4304U, 0x7EU, now++);
  BusWrite(snes, 0x420CU, 0x01U, now++);

  // Render through end of frame. RunToTime interleaves MachineSync with event
  // firing so each HDMA fire enqueues its INIDISP write into the PPU's lazy
  // log BEFORE the PPU's render dots for that scanline run. A single bulk
  // MachineSync would catch the PPU up past those write cycles before HDMA
  // had a chance to enqueue them.
  const TimeMasterT past_frame = 262U * 1364U + 200U;
  RunToTime(snes, past_frame);
  ppu.CatchUpTo(past_frame);

  const FrameBufferView view = ppu.BuildFrontView();

  // Pixel sampling helper: per Ppu::BrightnessScale, channel_out = (31 *
  // (b+1)) >> 4 for an input of 31. Recombine into BGR555.
  auto expect_white_at_brightness = [](uint8_t b) -> uint16_t {
    const uint32_t f = static_cast<uint32_t>(b) + 1U;
    const uint32_t ch = (31U * f) >> 4U;
    return static_cast<uint16_t>(ch | (ch << 5U) | (ch << 10U));
  };

  // V=5 (within V=0..7 band): brightness 15.
  REQUIRE(view.pixels[5U * view.stride + 0U] == expect_white_at_brightness(0x0FU));
  // V=12 (within V=8..15 band): brightness 8.
  REQUIRE(view.pixels[12U * view.stride + 0U] == expect_white_at_brightness(0x08U));
  // V=20 (within V=16..23 band): brightness 4.
  REQUIRE(view.pixels[20U * view.stride + 0U] == expect_white_at_brightness(0x04U));
}

TEST_CASE("HDMA init stalls master_time by 18 + 8 per active channel", "[unit][dma][hdma]") {
  SNES snes;
  snes.Reset();
  TimeMasterT now = 1;

  // Enable channels 0, 1, 4 (three channels = 3 bits set in HDMAEN).
  BusWrite(snes, 0x420CU, 0x13U, now++);

  // Just before init fires, master_time is whatever the last bus access set
  // (the BusWrite above bumped it). Snapshot, then fire init, then check.
  const TimeMasterT before = snes.GetMasterTime();
  REQUIRE(before < 24U);  // sanity: init hasn't fired yet

  snes.MachineSync(30U);
  snes.GetScheduler().FireEventsThrough(30U);

  // Init fires at master_time=24 and stalls for 18 + 8*3 = 42 master cycles.
  // The cumulative master_time should be at least 24 + 42 = 66.
  REQUIRE(snes.GetMasterTime() >= 24U + 18U + 8U * 3U);
}

TEST_CASE("HDMA per-line advances master_time by per-channel byte cost", "[unit][dma][hdma]") {
  SNES snes;
  snes.Reset();
  TimeMasterT now = 1;

  // 1-line entry; mode 0 (1 byte transfer); 1 active channel.
  BusWrite(snes, 0x7E0000U, 0x01U, now++);
  BusWrite(snes, 0x7E0001U, 0x05U, now++);
  BusWrite(snes, 0x7E0002U, 0x00U, now++);

  BusWrite(snes, 0x4300U, 0x00U, now++);
  BusWrite(snes, 0x4301U, 0x00U, now++);
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);
  BusWrite(snes, 0x4304U, 0x7EU, now++);
  BusWrite(snes, 0x420CU, 0x01U, now++);

  // Fire init only.
  snes.MachineSync(30U);
  snes.GetScheduler().FireEventsThrough(30U);
  const TimeMasterT after_init = snes.GetMasterTime();

  // Fire V=0 per-line. Cost: 8 (header read) + 8 (byte transfer) + 8 (overhead) = 24.
  snes.MachineSync(1200U);
  snes.GetScheduler().FireEventsThrough(1200U);

  REQUIRE(snes.GetMasterTime() >= 1096U + 24U);
  // Sanity: per-line stall happened on top of the init stall.
  REQUIRE(snes.GetMasterTime() > after_init);
}

TEST_CASE("HDMA re-arms on the next frame after the prior frame's stream ended",
          "[unit][dma][hdma]") {
  SNES snes;
  snes.Reset();
  Ppu& ppu = snes.GetPpu();
  TimeMasterT now = 1;

  // Short stream: 1-line entry writing 0x07 to INIDISP, then terminator. The
  // channel should finish well before V=224 in frame 0. Frame 1's init must
  // reset hdma_finished and the per-frame mask so the same stream runs again.
  BusWrite(snes, 0x7E0000U, 0x01U, now++);
  BusWrite(snes, 0x7E0001U, 0x07U, now++);
  BusWrite(snes, 0x7E0002U, 0x00U, now++);

  BusWrite(snes, 0x4300U, 0x00U, now++);
  BusWrite(snes, 0x4301U, 0x00U, now++);  // BBAD = $00 (INIDISP)
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);
  BusWrite(snes, 0x4304U, 0x7EU, now++);
  BusWrite(snes, 0x420CU, 0x01U, now++);

  // --- Frame 0 ---
  // Drive past V=2 H=274 so frame 0's terminator-load fires and the channel
  // marks itself finished. Capture the brightness reached this frame.
  const TimeMasterT mid_frame_0 = 1096U + 2U * 1364U + 200U;
  RunToTime(snes, mid_frame_0);
  ppu.CatchUpTo(mid_frame_0);
  REQUIRE(snes.GetDma().GetChannelState(0).hdma_finished == true);
  REQUIRE(ppu.GetBrightness() == 0x07U);

  // Sanity: corrupt INIDISP between frames so the next frame's HDMA write is
  // observably distinct from the residual frame-0 value. Without re-init, the
  // channel would stay finished and this poke would persist through frame 1.
  BusWrite(snes, 0x002100U, 0x00U, mid_frame_0 + 1U);  // brightness 0
  ppu.CatchUpTo(mid_frame_0 + 100U);
  REQUIRE(ppu.GetBrightness() == 0x00U);

  // --- Frame 1 ---
  // The next HDMA init fires at 262*1364 + 24 = 357392. Drive past it AND
  // past V=0 H=274 of frame 1 so the channel reloads the same source table
  // and writes 0x07 again.
  const TimeMasterT into_frame_1 = 262U * 1364U + 1096U + 200U;
  RunToTime(snes, into_frame_1);
  ppu.CatchUpTo(into_frame_1);

  // Frame 1's init should have rebuilt the channel: A2A back to 0, finished
  // cleared. After V=0 fires, the source-table walk advances A2A and INIDISP
  // becomes 0x07 once more.
  REQUIRE(snes.GetDma().GetChannelState(0).hdma_finished == false);
  REQUIRE(ppu.GetBrightness() == 0x07U);
}

TEST_CASE("HDMA chains across frame boundary: next init scheduled after V=224", "[unit][dma][hdma]") {
  SNES snes;
  snes.Reset();
  TimeMasterT now = 1;
  BusWrite(snes, 0x420CU, 0x01U, now++);

  // Drive through all 225 per-line fires for one frame: init at 24, V=0..224
  // at 1096 + V*1364. The last per-line is at 1096 + 224*1364 = 306632.
  const TimeMasterT past_last_line = 1096U + 224U * 1364U + 50U;
  snes.MachineSync(past_last_line);
  snes.GetScheduler().FireEventsThrough(past_last_line);

  // Next HDMA event should be the next frame's init at 262*1364 + 24 = 357392.
  const TimeMasterT next_init = 262U * 1364U + 24U;
  REQUIRE(EarliestSignal(snes, SignalKind::kHdmaFire) == next_init);
}

TEST_CASE("HDMA mode 1 transfers 2 bytes per unit advancing A2A by 2", "[unit][dma][hdma]") {
  SNES snes;
  snes.Reset();
  TimeMasterT now = 1;

  // Single 1-line entry with two bytes (one mode-1 unit). Terminator follows.
  BusWrite(snes, 0x7E0000U, 0x01U, now++);
  BusWrite(snes, 0x7E0001U, 0xAAU, now++);
  BusWrite(snes, 0x7E0002U, 0xBBU, now++);
  BusWrite(snes, 0x7E0003U, 0x00U, now++);

  BusWrite(snes, 0x4300U, 0x01U, now++);  // DMAP: direct, mode 1
  BusWrite(snes, 0x4301U, 0x0DU, now++);  // BBAD: $210D / $210E
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);
  BusWrite(snes, 0x4304U, 0x7EU, now++);
  BusWrite(snes, 0x420CU, 0x01U, now++);

  snes.MachineSync(1500U);
  snes.GetScheduler().FireEventsThrough(1500U);

  // After V=0 line: header at $7E:0000 = 0x01 (1 line, no repeat); reload
  // advances A2A to 1. Mode 1 transfers 2 bytes from $7E:0001-0002, A2A → 3.
  // NTRL decrements to 0; do_transfer = true (next line will reload).
  REQUIRE(snes.GetDma().GetChannelState(0).a2a == 0x03U);
  REQUIRE(snes.GetDma().GetChannelState(0).ntrl == 0x00U);
  REQUIRE(snes.GetDma().GetChannelState(0).hdma_do_transfer == true);
}

TEST_CASE("HDMA repeat-each-line transfers a fresh byte every scanline", "[unit][dma][hdma]") {
  SNES snes;
  snes.Reset();
  Ppu& ppu = snes.GetPpu();
  TimeMasterT now = 1;

  // Header 0x83: bit 7 = repeat, lines = 3. Three bytes follow, transferred
  // on lines 0, 1, 2; line 3 hits the terminator.
  BusWrite(snes, 0x7E0000U, 0x83U, now++);
  BusWrite(snes, 0x7E0001U, 0x01U, now++);
  BusWrite(snes, 0x7E0002U, 0x02U, now++);
  BusWrite(snes, 0x7E0003U, 0x03U, now++);
  BusWrite(snes, 0x7E0004U, 0x00U, now++);

  BusWrite(snes, 0x4300U, 0x00U, now++);  // mode 0
  BusWrite(snes, 0x4301U, 0x00U, now++);  // BBAD = $2100
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);
  BusWrite(snes, 0x4304U, 0x7EU, now++);
  BusWrite(snes, 0x420CU, 0x01U, now++);

  // After 3 line fires the last byte (0x03) should be the most recent INIDISP write.
  const TimeMasterT three_lines = 1096U + 2U * 1364U + 200U;  // V=0,1,2 H=274 + slack
  snes.MachineSync(three_lines);
  snes.GetScheduler().FireEventsThrough(three_lines);
  ppu.CatchUpTo(three_lines + 50U);

  REQUIRE(ppu.GetBrightness() == 0x03U);
  // After three transfers, A2A advanced past header + 3 data bytes = 4.
  REQUIRE(snes.GetDma().GetChannelState(0).a2a == 0x04U);
  // NTRL: started at 0x83, decremented three times to 0x80 | 0 = 0x80.
  REQUIRE(snes.GetDma().GetChannelState(0).ntrl == 0x80U);
}

TEST_CASE("HDMA channel terminates on count-byte 0 and no further transfers fire", "[unit][dma][hdma]") {
  SNES snes;
  snes.Reset();
  Ppu& ppu = snes.GetPpu();
  TimeMasterT now = 1;

  // Single-line entry then terminator. Line 0 transfers 0x05; line 1 hits 0,
  // channel finishes. After line 1, INIDISP should still read back as 0x05.
  BusWrite(snes, 0x7E0000U, 0x01U, now++);
  BusWrite(snes, 0x7E0001U, 0x05U, now++);
  BusWrite(snes, 0x7E0002U, 0x00U, now++);
  // Garbage in the slot past the terminator — should never be read.
  BusWrite(snes, 0x7E0003U, 0xFFU, now++);

  BusWrite(snes, 0x4300U, 0x00U, now++);
  BusWrite(snes, 0x4301U, 0x00U, now++);
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);
  BusWrite(snes, 0x4304U, 0x7EU, now++);
  BusWrite(snes, 0x420CU, 0x01U, now++);

  // Drive past V=2 to let line 1 hit the terminator.
  const TimeMasterT past_terminator = 1096U + 2U * 1364U + 200U;
  snes.MachineSync(past_terminator);
  snes.GetScheduler().FireEventsThrough(past_terminator);
  ppu.CatchUpTo(past_terminator + 50U);

  REQUIRE(snes.GetDma().GetChannelState(0).hdma_finished == true);
  REQUIRE(ppu.GetBrightness() == 0x05U);  // The 0xFF byte should NEVER have been written.
}

TEST_CASE("HDMA init skips channels with HDMAEN bit clear", "[unit][dma][hdma]") {
  SNES snes;
  snes.Reset();
  TimeMasterT now = 1;

  // Channel 0 enabled, channel 1 not. Both have non-zero A1T to prove that
  // only channel 0's a2a gets initialized.
  BusWrite(snes, 0x4302U, 0xAAU, now++);
  BusWrite(snes, 0x4303U, 0xBBU, now++);
  BusWrite(snes, 0x4312U, 0xCCU, now++);
  BusWrite(snes, 0x4313U, 0xDDU, now++);

  BusWrite(snes, 0x420CU, 0x01U, now++);  // only ch 0
  snes.MachineSync(30U);
  snes.GetScheduler().FireEventsThrough(30U);

  REQUIRE(snes.GetDma().GetChannelState(0).a2a == 0xAAU);
  REQUIRE(snes.GetDma().GetChannelState(0).a2a_high == 0xBBU);
  // Channel 1: HDMA inactive, so a2a stays at its default (0).
  REQUIRE(snes.GetDma().GetChannelState(1).a2a == 0x00U);
  REQUIRE(snes.GetDma().GetChannelState(1).a2a_high == 0x00U);
}
