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
