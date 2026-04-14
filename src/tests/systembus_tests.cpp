#include <catch2/catch_test_macros.hpp>

#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"
#include "systembus_test_access.h"

#include <array>

using namespace pupsnes;

// --- Mock devices ---

class MockMemoryDevice : public Device {
  public:
    static constexpr std::size_t SIZE = 256;
    std::array<uint8_t, SIZE> memory{};

    explicit MockMemoryDevice(SNES *snes) : Device(snes) {}

    TickResult tick(time_master_delta_t budget) override { return {budget, TickStopReason::BudgetExhausted}; }
    void onEvent(const SchedulerEvent &) override {}

    uint8_t readRegister(uint32_t offset) override { return memory[offset % SIZE]; }
    void writeRegister(uint32_t offset, uint8_t data) override { memory[offset % SIZE] = data; }
};

class MockMMIODevice : public Device {
  public:
    uint32_t last_read_offset = 0;
    uint32_t last_write_offset = 0;
    uint8_t last_write_data = 0;
    uint8_t read_value = 0x42;
    int tick_calls = 0;

    explicit MockMMIODevice(SNES *snes) : Device(snes) {}

    TickResult tick(time_master_delta_t budget) override {
        ++tick_calls;
        return {budget, TickStopReason::BudgetExhausted};
    }
    void onEvent(const SchedulerEvent &) override {}

    uint8_t readRegister(uint32_t offset) override {
        last_read_offset = offset;
        return read_value;
    }
    void writeRegister(uint32_t offset, uint8_t data) override {
        last_write_offset = offset;
        last_write_data = data;
    }
};

// --- Tests ---

TEST_CASE("mapPage and unmapPage populate and clear entries", "[unit]") {
    SNES snes;
    MockMemoryDevice dev(&snes);

    snes.system_bus->mapPage({0x00, 0x21, dev.getDeviceId(), 0, PageDeviceKind::SameClockMMIO, 8});

    auto &entry = SystemBusTestAccess::getPageEntry(*snes.system_bus, 0x00, 0x21);
    REQUIRE(entry.device_id == dev.getDeviceId());
    REQUIRE(entry.kind == PageDeviceKind::SameClockMMIO);
    REQUIRE(entry.access_speed == 8);
    REQUIRE(entry.base_offset == 0);

    snes.system_bus->unmapPage(0x00, 0x21);
    auto &cleared = SystemBusTestAccess::getPageEntry(*snes.system_bus, 0x00, 0x21);
    REQUIRE(cleared.kind == PageDeviceKind::Unmapped);
}

TEST_CASE("plan decodes address to correct target and offset", "[unit]") {
    SNES snes;
    MockMemoryDevice dev(&snes);

    // Map page at bank 0x7E, page 0x00 with base_offset 0
    snes.system_bus->mapPage({0x7E, 0x00, dev.getDeviceId(), 0, PageDeviceKind::Memory, 8});

    BusPlan p = snes.system_bus->plan(0x7E0042, BusAccessType::Read);
    REQUIRE(p.outcome == BusPlanOutcome::InlineComplete);
    REQUIRE(p.target_device == dev.getDeviceId());
    REQUIRE(p.device_offset == 0x42);
    REQUIRE(p.original_address == 0x7E0042);
    REQUIRE(p.access_cycles == 8);
}

TEST_CASE("plan for unmapped address returns Rejected", "[unit]") {
    SNES snes;

    BusPlan p = snes.system_bus->plan(0x100000, BusAccessType::Read);
    REQUIRE(p.outcome == BusPlanOutcome::Rejected);
}

TEST_CASE("plan is pure and idempotent", "[unit]") {
    SNES snes;
    MockMMIODevice dev(&snes);

    snes.system_bus->mapPage({0x00, 0x21, dev.getDeviceId(), 0x100, PageDeviceKind::SameClockMMIO, 6});

    BusPlan p1 = snes.system_bus->plan(0x002105, BusAccessType::Read);
    BusPlan p2 = snes.system_bus->plan(0x002105, BusAccessType::Read);

    REQUIRE(p1.outcome == p2.outcome);
    REQUIRE(p1.target_device == p2.target_device);
    REQUIRE(p1.device_offset == p2.device_offset);
    REQUIRE(p1.access_cycles == p2.access_cycles);
    REQUIRE(dev.tick_calls == 0);
}

TEST_CASE("follow InlineComplete read/write for Memory", "[unit]") {
    SNES snes;
    MockMemoryDevice dev(&snes);

    snes.system_bus->mapPage({0x7E, 0x00, dev.getDeviceId(), 0, PageDeviceKind::Memory, 8});

    // Pre-fill a byte
    dev.memory[0x10] = 0xAB;

    // Read
    BusPlan read_plan = snes.system_bus->plan(0x7E0010, BusAccessType::Read);
    BusFollowResult read_result = snes.system_bus->follow(read_plan, 0, dev.getDeviceId());
    REQUIRE(read_result.outcome == BusPlanOutcome::InlineComplete);
    REQUIRE(read_result.data == 0xAB);

    // Write
    BusPlan write_plan = snes.system_bus->plan(0x7E0010, BusAccessType::Write, 0xCD);
    BusFollowResult write_result = snes.system_bus->follow(write_plan, 0, dev.getDeviceId());
    REQUIRE(write_result.outcome == BusPlanOutcome::InlineComplete);
    REQUIRE(dev.memory[0x10] == 0xCD);
}

TEST_CASE("follow InlineComplete for SameClockMMIO triggers catch-up", "[unit]") {
    SNES snes;
    MockMMIODevice mmio_dev(&snes);

    snes.system_bus->mapPage({0x00, 0x21, mmio_dev.getDeviceId(), 0x100, PageDeviceKind::SameClockMMIO, 6});

    // Device starts at time 0; we access at time 100
    REQUIRE(mmio_dev.getTime() == 0);

    BusPlan p = snes.system_bus->plan(0x002100, BusAccessType::Read);
    BusFollowResult result = snes.system_bus->follow(p, 100, mmio_dev.getDeviceId());

    REQUIRE(result.outcome == BusPlanOutcome::InlineComplete);
    REQUIRE(result.data == 0x42);       // MockMMIODevice returns 0x42
    REQUIRE(mmio_dev.getTime() == 100); // Caught up
    REQUIRE(mmio_dev.tick_calls == 1);
    REQUIRE(mmio_dev.last_read_offset == 0x100); // base_offset + (0x00 & 0xFF)
}

TEST_CASE("follow InlineComplete for SameClockMMIO write", "[unit]") {
    SNES snes;
    MockMMIODevice mmio_dev(&snes);

    snes.system_bus->mapPage({0x00, 0x21, mmio_dev.getDeviceId(), 0, PageDeviceKind::SameClockMMIO, 6});

    BusPlan p = snes.system_bus->plan(0x002105, BusAccessType::Write, 0xFF);
    BusFollowResult result = snes.system_bus->follow(p, 50, mmio_dev.getDeviceId());

    REQUIRE(result.outcome == BusPlanOutcome::InlineComplete);
    REQUIRE(mmio_dev.getTime() == 50);
    REQUIRE(mmio_dev.last_write_offset == 0x05);
    REQUIRE(mmio_dev.last_write_data == 0xFF);
}

TEST_CASE("follow ScheduledComplete for CrossClockMMIO creates token", "[unit]") {
    SNES snes;
    MockMMIODevice source_dev(&snes);
    MockMMIODevice target_dev(&snes);

    snes.system_bus->mapPage({0x00, 0x21, target_dev.getDeviceId(), 0, PageDeviceKind::CrossClockMMIO, 12});

    BusPlan p = snes.system_bus->plan(0x002140, BusAccessType::Write, 0xAA);
    REQUIRE(p.outcome == BusPlanOutcome::ScheduledComplete);

    BusFollowResult result = snes.system_bus->follow(p, 200, source_dev.getDeviceId());

    REQUIRE(result.outcome == BusPlanOutcome::ScheduledComplete);
    REQUIRE(result.token != 0);

    // Verify token was created in the scheduler
    const Token *token = snes.scheduler->getToken(result.token);
    REQUIRE(token != nullptr);
    REQUIRE(token->type == TokenType::BusWrite);
    REQUIRE(token->completion_time == 212); // 200 + 12
    REQUIRE(token->address == 0x002140);
    REQUIRE(token->data == 0xAA);
}

TEST_CASE("follow Rejected plan returns Rejected result", "[unit]") {
    SNES snes;

    BusPlan p = snes.system_bus->plan(0x100000, BusAccessType::Read);
    REQUIRE(p.outcome == BusPlanOutcome::Rejected);

    BusFollowResult result = snes.system_bus->follow(p, 0, 0);
    REQUIRE(result.outcome == BusPlanOutcome::Rejected);
}

TEST_CASE("page table mirrors: two entries point to same device at different offsets", "[unit]") {
    SNES snes;
    MockMemoryDevice dev(&snes);

    // Map two pages to the same device with different base_offsets
    snes.system_bus->mapPage({0x00, 0x00, dev.getDeviceId(), 0, PageDeviceKind::Memory, 8});
    snes.system_bus->mapPage({0x80, 0x00, dev.getDeviceId(), 0, PageDeviceKind::Memory, 8});

    // Write via first mirror
    dev.memory[0x10] = 0xEE;

    // Read via second mirror
    BusPlan p = snes.system_bus->plan(0x800010, BusAccessType::Read);
    BusFollowResult result = snes.system_bus->follow(p, 0, dev.getDeviceId());

    REQUIRE(result.data == 0xEE);
}

TEST_CASE("catch-up does not tick device already at or past target time", "[unit]") {
    SNES snes;
    MockMMIODevice mmio_dev(&snes);

    snes.system_bus->mapPage({0x00, 0x21, mmio_dev.getDeviceId(), 0, PageDeviceKind::SameClockMMIO, 6});

    // Advance device past the access time
    mmio_dev.advanceLocalTime(200);
    REQUIRE(mmio_dev.getTime() == 200);

    BusPlan p = snes.system_bus->plan(0x002100, BusAccessType::Read);
    BusFollowResult result = snes.system_bus->follow(p, 100, mmio_dev.getDeviceId());

    REQUIRE(result.outcome == BusPlanOutcome::InlineComplete);
    REQUIRE(mmio_dev.tick_calls == 0);  // No catch-up needed
    REQUIRE(mmio_dev.getTime() == 200); // Unchanged
}

TEST_CASE("multiple sequential plan/follow operations", "[unit]") {
    SNES snes;
    MockMemoryDevice dev(&snes);

    snes.system_bus->mapPage({0x7E, 0x00, dev.getDeviceId(), 0, PageDeviceKind::Memory, 8});

    // Write then read at two different offsets
    BusPlan w1 = snes.system_bus->plan(0x7E0000, BusAccessType::Write, 0x11);
    snes.system_bus->follow(w1, 0, dev.getDeviceId());

    BusPlan w2 = snes.system_bus->plan(0x7E0001, BusAccessType::Write, 0x22);
    snes.system_bus->follow(w2, 0, dev.getDeviceId());

    BusPlan r1 = snes.system_bus->plan(0x7E0000, BusAccessType::Read);
    BusFollowResult res1 = snes.system_bus->follow(r1, 0, dev.getDeviceId());

    BusPlan r2 = snes.system_bus->plan(0x7E0001, BusAccessType::Read);
    BusFollowResult res2 = snes.system_bus->follow(r2, 0, dev.getDeviceId());

    REQUIRE(res1.data == 0x11);
    REQUIRE(res2.data == 0x22);
}
