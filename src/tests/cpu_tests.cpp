#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/cartridge.h"
#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"
#include "pupsnes/hw/wram.h"
#include "scheduler_test_access.h"

using namespace pupsnes;

class TestROM : public Device {
   public:
    static constexpr std::size_t SIZE = 512;
    std::array<uint8_t, SIZE> mem{};

    explicit TestROM(SNES* snes) : Device(snes) {}

    TickResult tick(time_master_delta_t budget) override { return {budget, TickStopReason::BudgetExhausted}; }
    void onEvent(const SchedulerEvent&) override {}

    uint8_t readRegister(uint32_t offset) override { return mem[offset % SIZE]; }
    void writeRegister(uint32_t offset, uint8_t data) override { mem[offset % SIZE] = data; }
};

class ObservedMMIO : public Device {
   public:
    static constexpr std::size_t SIZE = 512;
    std::array<uint8_t, SIZE> mem{};
    std::vector<time_master_t> read_times;

    explicit ObservedMMIO(SNES* snes) : Device(snes) {}

    TickResult tick(time_master_delta_t budget) override { return {budget, TickStopReason::BudgetExhausted}; }
    void onEvent(const SchedulerEvent&) override {}

    uint8_t readRegister(uint32_t offset) override {
        read_times.push_back(getTime());
        return mem[offset % SIZE];
    }
};

struct TestFixture {
    SNES snes;
    TestROM rom{&snes};
    CPU cpu{&snes};

    TestFixture() {
        snes.system_bus->mapPage({0x00, 0x80, rom.getDeviceId(), 0x000, PageDeviceKind::Memory, 8});
        snes.system_bus->mapPage({0x00, 0x81, rom.getDeviceId(), 0x100, PageDeviceKind::Memory, 8});

        auto r = cpu.regs();
        r.PC = 0x8000;
        cpu.setRegs(r);
    }

    void loadAt(uint16_t offset, std::initializer_list<uint8_t> bytes) {
        std::size_t i = offset & 0x1FFu;
        for (uint8_t b : bytes) {
            rom.mem[i++ & 0x1FFu] = b;
        }
    }
};

struct MMIOProgramFixture {
    SNES snes;
    ObservedMMIO program{&snes};
    CPU cpu{&snes};

    MMIOProgramFixture() {
        snes.system_bus->mapPage({0x00, 0x80, program.getDeviceId(), 0x000, PageDeviceKind::SameClockMMIO, 8});
        snes.system_bus->mapPage({0x00, 0x81, program.getDeviceId(), 0x100, PageDeviceKind::SameClockMMIO, 8});

        auto r = cpu.regs();
        r.PC = 0x8000;
        cpu.setRegs(r);
    }

    void loadAt(uint16_t offset, std::initializer_list<uint8_t> bytes) {
        std::size_t i = offset & 0x1FFu;
        for (uint8_t b : bytes) {
            program.mem[i++ & 0x1FFu] = b;
        }
    }
};

struct AsyncProgramFixture {
    SNES snes;
    TestROM async_target{&snes};
    CPU cpu{&snes};

    AsyncProgramFixture() {
        snes.system_bus->mapPage({0x00, 0x80, async_target.getDeviceId(), 0x000, PageDeviceKind::CrossClockMMIO, 8});

        auto r = cpu.regs();
        r.PC = 0x8000;
        cpu.setRegs(r);
    }
};

struct ResetFixture {
    SNES snes;
    Cartridge cartridge{&snes};
    WRAM wram{&snes};
    CPU cpu{&snes};
    std::array<uint8_t, Cartridge::kLoROMWindowSize> rom{};

    ResetFixture() {
        rom.fill(0xEA);
        setResetVector(0x8000);
        wram.mapSystemBus(*snes.system_bus);
        syncCartridge();
    }

    void setResetVector(uint16_t address) {
        rom[0x7FFCU] = static_cast<uint8_t>(address & 0x00FFU);
        rom[0x7FFDU] = static_cast<uint8_t>(address >> 8U);
    }

    void setRomByte(std::size_t offset, uint8_t value) { rom[offset] = value; }

    void syncCartridge() {
        cartridge.loadLoROM(rom);
        cartridge.mapLoROM(*snes.system_bus);
    }
};

TEST_CASE("CPU registers with SNES on construction", "[cpu]") {
    SNES snes;
    CPU cpu(&snes);

    REQUIRE(cpu.getDeviceId() != static_cast<device_id_t>(-1));
    REQUIRE(snes.getDevice(cpu.getDeviceId()) == &cpu);
}

TEST_CASE("CPU initial register state", "[cpu]") {
    SNES snes;
    CPU cpu(&snes);

    auto r = cpu.regs();
    REQUIRE(r.PC == 0x0000);
    REQUIRE(r.A == 0);
    REQUIRE(r.X == 0);
    REQUIRE(r.Y == 0);
    REQUIRE(r.SP == 0x01FF);
    REQUIRE(r.PBR == 0);
    REQUIRE(r.DBR == 0);
    REQUIRE(r.DP == 0);
    REQUIRE(r.P.E == true);
    REQUIRE(r.P.M == true);
    REQUIRE(r.P.X == true);
    REQUIRE(r.P.I == true);
    REQUIRE(cpu.getMicroOpIndex() == 0);
}

TEST_CASE("CPU reset fetches the reset vector through cartridge mapping", "[cpu]") {
    ResetFixture f;

    f.cpu.reset();

    REQUIRE(f.cpu.regs().PBR == 0);
    REQUIRE(f.cpu.regs().PC == 0x8000);
    REQUIRE(f.cpu.regs().SP == 0x01FF);
    REQUIRE(f.cpu.regs().P.E == true);
    REQUIRE(f.cpu.regs().P.M == true);
    REQUIRE(f.cpu.regs().P.X == true);
    REQUIRE(f.cpu.regs().P.I == true);
    REQUIRE(f.cpu.getMicroOpIndex() == 0);
    REQUIRE(f.cpu.getTime() == 0);
}

TEST_CASE("CPU reset clears in-flight execution state and resumes from the reset vector", "[cpu]") {
    ResetFixture f;
    f.setRomByte(0x0000U, 0xA9);
    f.setRomByte(0x0001U, 0x11);
    f.syncCartridge();

    auto regs = f.cpu.regs();
    regs.A = 0x00FF;
    regs.PC = 0x8123;
    regs.PBR = 0x7E;
    regs.P.D = true;
    regs.P.C = true;
    f.cpu.setRegs(regs);
    (void)f.cpu.tick(1);
    REQUIRE(f.cpu.getMicroOpIndex() == 1);

    f.cpu.reset();

    REQUIRE(f.cpu.regs().PC == 0x8000);
    REQUIRE(f.cpu.regs().PBR == 0);
    REQUIRE(f.cpu.regs().A == 0);
    REQUIRE(f.cpu.regs().P.D == false);
    REQUIRE(f.cpu.regs().P.C == false);
    REQUIRE(f.cpu.getMicroOpIndex() == 0);
}

TEST_CASE("CPU executes from the cartridge after reset", "[cpu]") {
    ResetFixture f;
    f.setRomByte(0x0000U, 0xA9);
    f.setRomByte(0x0001U, 0x42);
    f.syncCartridge();

    f.cpu.reset();
    TickResult r = f.cpu.tick(2);

    REQUIRE(r.completed_cycles == 2);
    REQUIRE(r.reason == TickStopReason::BudgetExhausted);
    REQUIRE(f.cpu.regs().PC == 0x8002);
    REQUIRE(static_cast<uint8_t>(f.cpu.regs().A) == 0x42);
}

TEST_CASE("BRA branches relative to the post-operand PC", "[cpu]") {
    ResetFixture f;
    f.setRomByte(0x0000U, 0x80);
    f.setRomByte(0x0001U, 0x02);
    f.setRomByte(0x0004U, 0xA9);
    f.setRomByte(0x0005U, 0x7F);
    f.syncCartridge();

    f.cpu.reset();
    TickResult r = f.cpu.tick(5);

    REQUIRE(r.completed_cycles == 5);
    REQUIRE(r.reason == TickStopReason::BudgetExhausted);
    REQUIRE(f.cpu.regs().PC == 0x8006);
    REQUIRE(static_cast<uint8_t>(f.cpu.regs().A) == 0x7F);
}

TEST_CASE("BRA supports negative displacements for tight loops", "[cpu]") {
    ResetFixture f;
    f.setRomByte(0x0000U, 0x80);
    f.setRomByte(0x0001U, 0xFE);
    f.syncCartridge();

    f.cpu.reset();
    TickResult r = f.cpu.tick(6);

    REQUIRE(r.completed_cycles == 6);
    REQUIRE(r.reason == TickStopReason::BudgetExhausted);
    REQUIRE(f.cpu.regs().PC == 0x8000);
}

TEST_CASE("STA long writes accumulator low byte to mapped WRAM", "[cpu]") {
    ResetFixture f;
    f.setRomByte(0x0000U, 0xA9);
    f.setRomByte(0x0001U, 0x5A);
    f.setRomByte(0x0002U, 0x8F);
    f.setRomByte(0x0003U, 0x00);
    f.setRomByte(0x0004U, 0x00);
    f.setRomByte(0x0005U, 0x7E);
    f.syncCartridge();

    f.cpu.reset();
    TickResult r = f.cpu.tick(7);

    REQUIRE(r.completed_cycles == 7);
    REQUIRE(r.reason == TickStopReason::BudgetExhausted);
    REQUIRE(static_cast<uint8_t>(f.cpu.regs().A) == 0x5A);
    REQUIRE(f.cpu.regs().PC == 0x8006);
    REQUIRE(f.wram.peek(0x0000) == 0x5A);
}

TEST_CASE("Direct CPU tick advances execution state but not committed device time", "[cpu]") {
    TestFixture f;
    f.loadAt(0, {0xEA, 0xA9, 0x42});

    TickResult r1 = f.cpu.tick(2);
    REQUIRE(r1.completed_cycles == 2);
    REQUIRE(r1.reason == TickStopReason::BudgetExhausted);
    REQUIRE(f.cpu.regs().PC == 0x8001);
    REQUIRE(f.cpu.getTime() == 0);

    TickResult r2 = f.cpu.tick(2);
    REQUIRE(r2.completed_cycles == 2);
    REQUIRE(f.cpu.regs().PC == 0x8003);
    REQUIRE(static_cast<uint8_t>(f.cpu.regs().A) == 0x42);
    REQUIRE(f.cpu.getTime() == 0);
}

TEST_CASE("NOP tick slices still compose correctly without local_time mutation", "[cpu]") {
    TestFixture f;
    f.loadAt(0, {0xEA, 0xEA});

    (void)f.cpu.tick(1);
    REQUIRE(f.cpu.getMicroOpIndex() == 1);
    REQUIRE(f.cpu.regs().PC == 0x8001);
    REQUIRE(f.cpu.getTime() == 0);

    (void)f.cpu.tick(1);
    REQUIRE(f.cpu.getMicroOpIndex() == 2);
    REQUIRE(f.cpu.regs().PC == 0x8001);
    REQUIRE(f.cpu.getTime() == 0);
}

TEST_CASE("LDA immediate updates A and flags through direct tick", "[cpu]") {
    TestFixture f;
    f.loadAt(0, {0xA9, 0x80});

    TickResult r = f.cpu.tick(2);
    REQUIRE(r.completed_cycles == 2);
    REQUIRE(static_cast<uint8_t>(f.cpu.regs().A) == 0x80);
    REQUIRE(f.cpu.regs().P.N == true);
    REQUIRE(f.cpu.regs().P.Z == false);
    REQUIRE(f.cpu.getTime() == 0);
}

TEST_CASE("Fetch from unmapped address returns open-bus value and does not mutate committed time", "[cpu]") {
    SNES snes;
    CPU cpu(&snes);

    TickResult r = cpu.tick(2);
    REQUIRE(r.completed_cycles == 2);
    REQUIRE(cpu.regs().PC == 0x0001);
    REQUIRE(cpu.getTime() == 0);
}

TEST_CASE("Consecutive same-tick bus accesses use increasing absolute timestamps", "[cpu]") {
    MMIOProgramFixture f;
    f.loadAt(0, {0xA9, 0x42});
    f.cpu.advanceLocalTime(100);

    TickResult r = f.cpu.tick(2);

    REQUIRE(r.completed_cycles == 2);
    REQUIRE(f.program.read_times == std::vector<time_master_t>{100, 101});
    REQUIRE(f.cpu.getTime() == 100);
}

TEST_CASE("Scheduler-driven CPU execution commits time and reschedules after BudgetExhausted", "[cpu]") {
    TestFixture f;
    f.loadAt(0, {0xEA, 0xA9, 0x42});

    f.snes.scheduler->scheduleDeviceRun(&f.cpu, 0);
    f.snes.scheduler->scheduleEvent(2, &f.rom, SchedulerPhase::WakeSample, EventType::DeviceBoundary);
    f.snes.scheduler->scheduleEvent(4, &f.rom, SchedulerPhase::WakeSample, EventType::DeviceBoundary);

    f.snes.scheduler->step();
    REQUIRE(f.cpu.regs().PC == 0x8001);
    REQUIRE(f.cpu.getTime() == 2);
    REQUIRE(SchedulerTestAccess::hasPendingRun(*f.snes.scheduler, f.cpu.getDeviceId()));
    REQUIRE(SchedulerTestAccess::pendingRunTime(*f.snes.scheduler, f.cpu.getDeviceId()) == 2);

    f.snes.scheduler->step();
    REQUIRE(f.snes.getMasterTime() == 2);

    f.snes.scheduler->step();
    REQUIRE(f.cpu.regs().PC == 0x8003);
    REQUIRE(static_cast<uint8_t>(f.cpu.regs().A) == 0x42);
    REQUIRE(f.cpu.getTime() == 4);
    REQUIRE(SchedulerTestAccess::pendingRunTime(*f.snes.scheduler, f.cpu.getDeviceId()) == 4);
}

TEST_CASE("Cross-clock token completion wakes a blocked CPU through authoritative run scheduling", "[cpu]") {
    AsyncProgramFixture f;

    f.snes.scheduler->scheduleDeviceRun(&f.cpu, 0);

    f.snes.scheduler->step();
    const auto blocked_token = SchedulerTestAccess::blockedToken(*f.snes.scheduler, f.cpu.getDeviceId());
    REQUIRE(blocked_token != 0);
    REQUIRE_FALSE(SchedulerTestAccess::hasPendingRun(*f.snes.scheduler, f.cpu.getDeviceId()));
    REQUIRE(f.cpu.getTime() == 0);

    f.snes.scheduler->step();
    REQUIRE(f.snes.getMasterTime() == 8);
    REQUIRE(SchedulerTestAccess::hasPendingRun(*f.snes.scheduler, f.cpu.getDeviceId()));
    REQUIRE(SchedulerTestAccess::pendingRunTime(*f.snes.scheduler, f.cpu.getDeviceId()) == 8);
}

TEST_CASE("Same-clock scheduler-driven fetches keep absolute bus timestamps and CPU time aligned", "[cpu]") {
    MMIOProgramFixture f;
    f.loadAt(0, {0xA9, 0x7F});

    f.snes.scheduler->scheduleDeviceRun(&f.cpu, 100);
    f.snes.scheduler->scheduleEvent(102, &f.program, SchedulerPhase::WakeSample, EventType::DeviceBoundary);

    f.snes.scheduler->step();

    REQUIRE(f.program.read_times == std::vector<time_master_t>{100, 101});
    REQUIRE(static_cast<uint8_t>(f.cpu.regs().A) == 0x7F);
    REQUIRE(f.cpu.getTime() == 102);
}

TEST_CASE("CpuFlags::toByte encodes all flags correctly", "[cpu]") {
    CpuFlags f{};
    f.N = true;
    f.V = true;
    f.M = true;
    f.X = true;
    f.D = true;
    f.I = true;
    f.Z = true;
    f.C = true;
    REQUIRE(f.toByte() == 0xFF);
}

TEST_CASE("CpuFlags::toByte with no flags set returns 0", "[cpu]") {
    CpuFlags f{};
    f.N = false;
    f.V = false;
    f.M = false;
    f.X = false;
    f.D = false;
    f.I = false;
    f.Z = false;
    f.C = false;
    REQUIRE(f.toByte() == 0x00);
}

TEST_CASE("CpuFlags::fromByte in native mode sets M and X from byte", "[cpu]") {
    CpuFlags f{};
    f.fromByte(0x00, false);
    REQUIRE(f.M == false);
    REQUIRE(f.X == false);

    f.fromByte(0x30, false);
    REQUIRE(f.M == true);
    REQUIRE(f.X == true);
}

TEST_CASE("CpuFlags::fromByte in emulation mode ignores M and X bits", "[cpu]") {
    CpuFlags f{};
    f.M = true;
    f.X = true;
    f.fromByte(0x00, true);
    REQUIRE(f.M == true);
    REQUIRE(f.X == true);
}
