#include <array>
#include <catch2/catch_test_macros.hpp>

#include "pupsnes/hw/cartridge.h"
#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"
#include "pupsnes/hw/wram.h"
#include "systembus_test_access.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)

class MockMemoryDevice : public Device {
 public:
  static constexpr std::size_t kSize = 256;
  std::array<uint8_t, kSize> memory{};

  explicit MockMemoryDevice(SNES* snes) : Device(snes) {}

  TickResult Tick(TimeMasterDeltaT budget) override { return {budget, TickStopReason::kBudgetExhausted}; }
  void OnEvent(const SchedulerEvent&) override {}

  MmioReadResult ReadRegister(uint32_t offset, TimeMasterT /*current_time*/) override {
    return {memory[offset % kSize], 0xFFU};
  }
  void WriteRegister(uint32_t offset, uint8_t data, TimeMasterT /*current_time*/) override {
    memory[offset % kSize] = data;
  }
};

class MockMMIODevice : public Device {
 public:
  uint32_t last_read_offset = 0;
  uint32_t last_write_offset = 0;
  uint8_t last_write_data = 0;
  uint8_t read_value = 0x42;
  int tick_calls = 0;

  explicit MockMMIODevice(SNES* snes) : Device(snes) {}

  TickResult Tick(TimeMasterDeltaT budget) override {
    ++tick_calls;
    return {budget, TickStopReason::kBudgetExhausted};
  }
  void OnEvent(const SchedulerEvent&) override {}

  MmioReadResult ReadRegister(uint32_t offset, TimeMasterT /*current_time*/) override {
    last_read_offset = offset;
    return {read_value, 0xFFU};
  }
  void WriteRegister(uint32_t offset, uint8_t data, TimeMasterT /*current_time*/) override {
    last_write_offset = offset;
    last_write_data = data;
  }
};

TEST_CASE("MapPage and UnmapPage populate and clear entries", "[unit]") {
  SNES snes;
  MockMemoryDevice dev(&snes);

  snes.system_bus->MapPage({0x00, 0x21, dev.GetDeviceId(), 0, PageDeviceKind::kSameClockMmio, 8});

  auto& entry = SystemBusTestAccess::GetPageEntry(*snes.system_bus, 0x00, 0x21);
  REQUIRE(entry.device_id == dev.GetDeviceId());
  REQUIRE(entry.kind == PageDeviceKind::kSameClockMmio);
  REQUIRE(entry.access_speed == 8);
  REQUIRE(entry.base_offset == 0);

  snes.system_bus->UnmapPage(0x00, 0x21);
  auto& cleared = SystemBusTestAccess::GetPageEntry(*snes.system_bus, 0x00, 0x21);
  REQUIRE(cleared.kind == PageDeviceKind::kUnmapped);
}

TEST_CASE("plan decodes address to correct target and offset", "[unit]") {
  SNES snes;
  MockMemoryDevice dev(&snes);

  snes.system_bus->MapPage({0x7E, 0x00, dev.GetDeviceId(), 0, PageDeviceKind::kMemory, 8});

  BusPlan p = snes.system_bus->Plan(0x7E0042, BusAccessType::kRead);
  REQUIRE(p.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE(p.target_device == dev.GetDeviceId());
  REQUIRE(p.device_offset == 0x42);
  REQUIRE(p.original_address == 0x7E0042);
  REQUIRE(p.access_cycles == 8);
}

TEST_CASE("plan for unmapped address returns Rejected", "[unit]") {
  SNES snes;

  BusPlan p = snes.system_bus->Plan(0x400000, BusAccessType::kRead);
  REQUIRE(p.outcome == BusPlanOutcome::kRejected);
}

TEST_CASE("plan is pure and idempotent", "[unit]") {
  SNES snes;
  MockMMIODevice dev(&snes);

  snes.system_bus->MapPage({0x00, 0x21, dev.GetDeviceId(), 0x100, PageDeviceKind::kSameClockMmio, 6});

  BusPlan p1 = snes.system_bus->Plan(0x002105, BusAccessType::kRead);
  BusPlan p2 = snes.system_bus->Plan(0x002105, BusAccessType::kRead);

  REQUIRE(p1.outcome == p2.outcome);
  REQUIRE(p1.target_device == p2.target_device);
  REQUIRE(p1.device_offset == p2.device_offset);
  REQUIRE(p1.access_cycles == p2.access_cycles);
  REQUIRE(dev.tick_calls == 0);
}

TEST_CASE("follow InlineComplete read/write for Memory", "[unit]") {
  SNES snes;
  MockMemoryDevice dev(&snes);

  snes.system_bus->MapPage({0x7E, 0x00, dev.GetDeviceId(), 0, PageDeviceKind::kMemory, 8});

  dev.memory[0x10] = 0xAB;

  BusPlan read_plan = snes.system_bus->Plan(0x7E0010, BusAccessType::kRead);
  BusFollowResult read_result = snes.system_bus->Follow(read_plan, 0, dev.GetDeviceId());
  REQUIRE(read_result.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE(read_result.data == 0xAB);

  BusPlan write_plan = snes.system_bus->Plan(0x7E0010, BusAccessType::kWrite, 0xCD);
  BusFollowResult write_result = snes.system_bus->Follow(write_plan, 0, dev.GetDeviceId());
  REQUIRE(write_result.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE(dev.memory[0x10] == 0xCD);
}

TEST_CASE("follow InlineComplete for SameClockMMIO triggers catch-up", "[unit]") {
  SNES snes;
  MockMMIODevice mmio_dev(&snes);

  snes.system_bus->MapPage({0x00, 0x21, mmio_dev.GetDeviceId(), 0x100, PageDeviceKind::kSameClockMmio, 6});

  REQUIRE(mmio_dev.GetTime() == 0);

  BusPlan p = snes.system_bus->Plan(0x002100, BusAccessType::kRead);
  BusFollowResult result = snes.system_bus->Follow(p, 100, mmio_dev.GetDeviceId());

  REQUIRE(result.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE(result.data == 0x42);        // MockMMIODevice returns 0x42
  REQUIRE(mmio_dev.GetTime() == 100);  // Caught up
  REQUIRE(mmio_dev.tick_calls == 1);
  REQUIRE(mmio_dev.last_read_offset == 0x100);  // base_offset + (0x00 & 0xFF)
}

TEST_CASE("follow InlineComplete for SameClockMMIO write does not catch up", "[unit]") {
  // Lazy-replay rule: writes to same-clock MMIO append to the target device's
  // pending-write log without forcing a catch-up. The device stays behind
  // until a later read (or its own Tick) drains the log.
  SNES snes;
  MockMMIODevice mmio_dev(&snes);

  snes.system_bus->MapPage({0x00, 0x21, mmio_dev.GetDeviceId(), 0, PageDeviceKind::kSameClockMmio, 6});

  BusPlan p = snes.system_bus->Plan(0x002105, BusAccessType::kWrite, 0xFF);
  BusFollowResult result = snes.system_bus->Follow(p, 50, mmio_dev.GetDeviceId());

  REQUIRE(result.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE(mmio_dev.GetTime() == 0);   // Device time unchanged — no catch-up.
  REQUIRE(mmio_dev.tick_calls == 0);  // No catch-up tick.
  REQUIRE(mmio_dev.last_write_offset == 0x05);
  REQUIRE(mmio_dev.last_write_data == 0xFF);
}

TEST_CASE("follow ScheduledComplete for CrossClockMMIO creates token", "[unit]") {
  SNES snes;
  MockMMIODevice source_dev(&snes);
  MockMMIODevice target_dev(&snes);

  snes.system_bus->MapPage({0x00, 0x21, target_dev.GetDeviceId(), 0, PageDeviceKind::kCrossClockMmio, 12});

  BusPlan p = snes.system_bus->Plan(0x002140, BusAccessType::kWrite, 0xAA);
  REQUIRE(p.outcome == BusPlanOutcome::kScheduledComplete);

  BusFollowResult result = snes.system_bus->Follow(p, 200, source_dev.GetDeviceId());

  REQUIRE(result.outcome == BusPlanOutcome::kScheduledComplete);
  REQUIRE(result.token != 0);

  // Verify token was created in the scheduler
  const Token* token = snes.scheduler->GetToken(result.token);
  REQUIRE(token != nullptr);
  REQUIRE(token->type == TokenType::kBusWrite);
  REQUIRE(token->completion_time == 212);  // 200 + 12
  REQUIRE(token->address == 0x002140);
  REQUIRE(token->data == 0xAA);
}

TEST_CASE("follow Rejected plan returns Rejected result", "[unit]") {
  SNES snes;

  BusPlan p = snes.system_bus->Plan(0x400000, BusAccessType::kRead);
  REQUIRE(p.outcome == BusPlanOutcome::kRejected);

  BusFollowResult result = snes.system_bus->Follow(p, 0, 0);
  REQUIRE(result.outcome == BusPlanOutcome::kRejected);
}

TEST_CASE("page table mirrors: two entries point to same device at different offsets", "[unit]") {
  SNES snes;
  MockMemoryDevice dev(&snes);

  snes.system_bus->MapPage({0x00, 0x00, dev.GetDeviceId(), 0, PageDeviceKind::kMemory, 8});
  snes.system_bus->MapPage({0x80, 0x00, dev.GetDeviceId(), 0, PageDeviceKind::kMemory, 8});

  dev.memory[0x10] = 0xEE;

  BusPlan p = snes.system_bus->Plan(0x800010, BusAccessType::kRead);
  BusFollowResult result = snes.system_bus->Follow(p, 0, dev.GetDeviceId());

  REQUIRE(result.data == 0xEE);
}

TEST_CASE("catch-up does not tick device already at or past target time", "[unit]") {
  SNES snes;
  MockMMIODevice mmio_dev(&snes);

  snes.system_bus->MapPage({0x00, 0x21, mmio_dev.GetDeviceId(), 0, PageDeviceKind::kSameClockMmio, 6});

  mmio_dev.AdvanceLocalTime(200);
  REQUIRE(mmio_dev.GetTime() == 200);

  BusPlan p = snes.system_bus->Plan(0x002100, BusAccessType::kRead);
  BusFollowResult result = snes.system_bus->Follow(p, 100, mmio_dev.GetDeviceId());

  REQUIRE(result.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE(mmio_dev.tick_calls == 0);   // No catch-up needed
  REQUIRE(mmio_dev.GetTime() == 200);  // Unchanged
}

TEST_CASE("multiple sequential plan/follow operations", "[unit]") {
  SNES snes;
  MockMemoryDevice dev(&snes);

  snes.system_bus->MapPage({0x7E, 0x00, dev.GetDeviceId(), 0, PageDeviceKind::kMemory, 8});

  BusPlan w1 = snes.system_bus->Plan(0x7E0000, BusAccessType::kWrite, 0x11);
  snes.system_bus->Follow(w1, 0, dev.GetDeviceId());

  BusPlan w2 = snes.system_bus->Plan(0x7E0001, BusAccessType::kWrite, 0x22);
  snes.system_bus->Follow(w2, 0, dev.GetDeviceId());

  BusPlan r1 = snes.system_bus->Plan(0x7E0000, BusAccessType::kRead);
  BusFollowResult res1 = snes.system_bus->Follow(r1, 0, dev.GetDeviceId());

  BusPlan r2 = snes.system_bus->Plan(0x7E0001, BusAccessType::kRead);
  BusFollowResult res2 = snes.system_bus->Follow(r2, 0, dev.GetDeviceId());

  REQUIRE(res1.data == 0x11);
  REQUIRE(res2.data == 0x22);
}

TEST_CASE("WRAM maps banks 7E and 7F as contiguous memory", "[unit]") {
  SNES snes;
  WRAM& wram = snes.GetWram();

  BusPlan write_first_bank = snes.system_bus->Plan(0x7E0001, BusAccessType::kWrite, 0x12);
  BusPlan write_second_bank = snes.system_bus->Plan(0x7F0002, BusAccessType::kWrite, 0x34);
  (void)snes.system_bus->Follow(write_first_bank, 0, wram.GetDeviceId());
  (void)snes.system_bus->Follow(write_second_bank, 0, wram.GetDeviceId());

  BusFollowResult read_first_bank =
      snes.system_bus->Follow(snes.system_bus->Plan(0x7E0001, BusAccessType::kRead), 0, wram.GetDeviceId());
  BusFollowResult read_second_bank =
      snes.system_bus->Follow(snes.system_bus->Plan(0x7F0002, BusAccessType::kRead), 0, wram.GetDeviceId());

  REQUIRE(read_first_bank.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE(read_second_bank.outcome == BusPlanOutcome::kInlineComplete);
  REQUIRE(read_first_bank.data == 0x12);
  REQUIRE(read_second_bank.data == 0x34);
  REQUIRE(wram.Peek(0x0001) == 0x12);
  REQUIRE(wram.Peek(0x10002) == 0x34);
}

TEST_CASE("Cartridge LoROM mapping exposes reset vector and program window", "[unit]") {
  SNES snes;
  Cartridge& cartridge = snes.GetCartridge();
  std::array<uint8_t, Cartridge::kLoROMWindowSize> rom{};

  rom.fill(0xFF);
  rom[0x0000] = 0xA9;
  rom[0x0001] = 0x66;
  rom[0x7FFC] = 0x00;
  rom[0x7FFD] = 0x80;

  snes.LoadLoRom(rom);

  BusFollowResult reset_lo =
      snes.system_bus->Follow(snes.system_bus->Plan(0x00FFFC, BusAccessType::kRead), 0, cartridge.GetDeviceId());
  BusFollowResult reset_hi =
      snes.system_bus->Follow(snes.system_bus->Plan(0x00FFFD, BusAccessType::kRead), 0, cartridge.GetDeviceId());
  BusFollowResult opcode =
      snes.system_bus->Follow(snes.system_bus->Plan(0x008000, BusAccessType::kRead), 0, cartridge.GetDeviceId());

  REQUIRE(reset_lo.data == 0x00);
  REQUIRE(reset_hi.data == 0x80);
  REQUIRE(opcode.data == 0xA9);
}
