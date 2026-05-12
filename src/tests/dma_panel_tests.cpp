#include <catch2/catch_test_macros.hpp>

#include "pupsnes/hw/5a22/cpu_mmio.h"
#include "pupsnes/hw/dma_controller.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"

using namespace pupsnes;  // NOLINT

namespace {

void BusWrite(SNES& snes, SnesAddrT address, uint8_t data, TimeMasterT now) {
  BusPlan plan = snes.system_bus->Plan(address, BusAccessType::kWrite, data);
  REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
  (void)snes.system_bus->Follow(plan, now, 0);
}

// Channel-0 mode-1 A->B transfer setup, parameterized on DAS so different
// tests can probe boundary cases (64K, multi-byte, single-byte) without
// duplicating the boilerplate.
void ProgramChannel0(SNES& snes, uint16_t das, TimeMasterT& now) {
  BusWrite(snes, 0x4300U, 0x01U, now++);  // DMAP: mode 1, A->B, increment
  BusWrite(snes, 0x4301U, 0x18U, now++);  // BBAD = $18 (VMDATAL)
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);  // a1t = 0
  BusWrite(snes, 0x4304U, 0x7EU, now++);  // a1b = $7E
  BusWrite(snes, 0x4305U, static_cast<uint8_t>(das & 0xFFU), now++);
  BusWrite(snes, 0x4306U, static_cast<uint8_t>((das >> 8U) & 0xFFU), now++);
}

}  // namespace

TEST_CASE("DMA trigger ring captures one record per $420B write", "[unit][debugger][dma]") {
  SNES snes;
  TimeMasterT now = 100;
  ProgramChannel0(snes, 32U, now);

  REQUIRE(snes.GetDma().GetRecentTriggerCount() == 0U);
  BusWrite(snes, 0x420BU, 0x01U, now++);

  const DmaController& dma = snes.GetDma();
  REQUIRE(dma.GetRecentTriggerCount() == 1U);
  const auto& rec = dma.GetRecentTrigger(0);
  REQUIRE(rec.channels_mask == 0x01U);
  REQUIRE(rec.per_channel[0].dmap == 0x01U);
  REQUIRE(rec.per_channel[0].bbad == 0x18U);
  REQUIRE(rec.per_channel[0].a1b == 0x7EU);
  REQUIRE(rec.per_channel[0].a1t_start == 0x0000U);
  REQUIRE(rec.per_channel[0].das_start == 32U);
  REQUIRE(rec.per_channel[0].bytes_transferred == 32U);
  REQUIRE(rec.end_time > rec.start_time);
}

TEST_CASE("DMA trigger ring reports 65536 bytes when das_start is zero", "[unit][debugger][dma]") {
  SNES snes;
  TimeMasterT now = 100;
  // Program ch0 but leave DAS = 0 so the hardware "0 = 64K" semantics apply.
  // We don't trigger the 64K transfer through Trigger() — that would be slow;
  // instead, drive Trigger() directly with a tiny start_time and assert on the
  // captured record. The full-byte transfer still runs synchronously, but on a
  // freshly-constructed SNES the A-bus reads return open-bus and the B-bus
  // writes go to VRAM via the PPU's lazy-replay log, both cheap.
  ProgramChannel0(snes, 0U, now);
  BusWrite(snes, 0x420BU, 0x01U, now++);

  const auto& rec = snes.GetDma().GetRecentTrigger(0);
  REQUIRE(rec.per_channel[0].das_start == 0U);
  REQUIRE(rec.per_channel[0].bytes_transferred == 0x10000U);
}

TEST_CASE("DMA trigger ring populates only channels in the mask", "[unit][debugger][dma]") {
  SNES snes;
  TimeMasterT now = 100;

  // Channel 0: 4-byte transfer.
  BusWrite(snes, 0x4300U, 0x00U, now++);  // mode 0
  BusWrite(snes, 0x4301U, 0x18U, now++);
  BusWrite(snes, 0x4302U, 0x00U, now++);
  BusWrite(snes, 0x4303U, 0x00U, now++);
  BusWrite(snes, 0x4304U, 0x7EU, now++);
  BusWrite(snes, 0x4305U, 0x04U, now++);
  BusWrite(snes, 0x4306U, 0x00U, now++);

  // Channel 5: 8-byte transfer to a different B-port.
  BusWrite(snes, 0x4350U, 0x00U, now++);
  BusWrite(snes, 0x4351U, 0x22U, now++);
  BusWrite(snes, 0x4352U, 0x10U, now++);
  BusWrite(snes, 0x4353U, 0x00U, now++);
  BusWrite(snes, 0x4354U, 0x7EU, now++);
  BusWrite(snes, 0x4355U, 0x08U, now++);
  BusWrite(snes, 0x4356U, 0x00U, now++);

  BusWrite(snes, 0x420BU, 0x21U, now++);  // ch0 + ch5

  const auto& rec = snes.GetDma().GetRecentTrigger(0);
  REQUIRE(rec.channels_mask == 0x21U);
  REQUIRE(rec.per_channel[0].bytes_transferred == 4U);
  REQUIRE(rec.per_channel[0].bbad == 0x18U);
  REQUIRE(rec.per_channel[5].bytes_transferred == 8U);
  REQUIRE(rec.per_channel[5].bbad == 0x22U);
  REQUIRE(rec.per_channel[5].a1t_start == 0x0010U);
  // Channels not in the mask remain default-initialized.
  REQUIRE(rec.per_channel[1].bytes_transferred == 0U);
  REQUIRE(rec.per_channel[2].bytes_transferred == 0U);
  REQUIRE(rec.per_channel[3].bytes_transferred == 0U);
  REQUIRE(rec.per_channel[4].bytes_transferred == 0U);
  REQUIRE(rec.per_channel[6].bytes_transferred == 0U);
  REQUIRE(rec.per_channel[7].bytes_transferred == 0U);
}

TEST_CASE("DMA trigger ring wraps at capacity, oldest entries fall off", "[unit][debugger][dma]") {
  SNES snes;
  TimeMasterT now = 1000;
  // 1-byte transfers, ch0, so each Trigger() is cheap.
  ProgramChannel0(snes, 1U, now);

  constexpr std::size_t kCapacity = DmaController::kTriggerRingCapacity;
  constexpr std::size_t kExtraTriggers = 3U;
  for (std::size_t i = 0; i < kCapacity + kExtraTriggers; ++i) {
    // Re-program DAS each iteration (it gets decremented by the prior trigger).
    BusWrite(snes, 0x4305U, 0x01U, now++);
    BusWrite(snes, 0x4306U, 0x00U, now++);
    // Tag each trigger with its loop index via start_time so we can assert
    // which records survived the wrap. The bus-write timestamp threads through
    // CpuMmio into Trigger() as start_time.
    BusWrite(snes, 0x420BU, 0x01U, /*now=*/10'000U + i);
  }

  const DmaController& dma = snes.GetDma();
  REQUIRE(dma.GetRecentTriggerCount() == kCapacity);
  // Oldest surviving record should be from iteration kExtraTriggers
  // (0..kExtraTriggers-1 fell off).
  REQUIRE(dma.GetRecentTrigger(0).start_time == 10'000U + kExtraTriggers);
  REQUIRE(dma.GetRecentTrigger(kCapacity - 1U).start_time == 10'000U + kCapacity + kExtraTriggers - 1U);
}

TEST_CASE("DMA Reset() clears the trigger ring and last-mask", "[unit][debugger][dma]") {
  SNES snes;
  TimeMasterT now = 100;
  ProgramChannel0(snes, 4U, now);
  BusWrite(snes, 0x420BU, 0x01U, now++);
  REQUIRE(snes.GetDma().GetRecentTriggerCount() == 1U);
  REQUIRE(snes.GetDma().GetLastTriggerMask() == 0x01U);

  snes.Reset();

  REQUIRE(snes.GetDma().GetRecentTriggerCount() == 0U);
  REQUIRE(snes.GetDma().GetLastTriggerMask() == 0x00U);
}

TEST_CASE("DMA GetLastTriggerMask reflects the most recent $420B write", "[unit][debugger][dma]") {
  SNES snes;
  TimeMasterT now = 100;
  ProgramChannel0(snes, 1U, now);
  BusWrite(snes, 0x420BU, 0x01U, now++);
  REQUIRE(snes.GetDma().GetLastTriggerMask() == 0x01U);

  // Re-arm ch0 (DAS got consumed). Mask 0x80 still hits ch7 (unprogrammed,
  // DAS = 0 → 64K cycle which is fine; we only care about the mask field).
  // To keep the test fast, program ch7 with DAS=1 first.
  BusWrite(snes, 0x4375U, 0x01U, now++);
  BusWrite(snes, 0x4376U, 0x00U, now++);
  BusWrite(snes, 0x420BU, 0x80U, now++);
  REQUIRE(snes.GetDma().GetLastTriggerMask() == 0x80U);
}

TEST_CASE("CpuMmio exposes $420C HDMAEN shadow via GetHdmaEn", "[unit][debugger][dma]") {
  SNES snes;
  TimeMasterT now = 1;
  REQUIRE(snes.GetCpuMmio().GetHdmaEn() == 0x00U);
  BusWrite(snes, 0x420CU, 0xA5U, now++);
  REQUIRE(snes.GetCpuMmio().GetHdmaEn() == 0xA5U);
}
