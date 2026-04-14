#include <catch2/catch_test_macros.hpp>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"

#include <array>
#include <cstdint>

using namespace pupsnes;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

// A flat ROM/RAM device used to back test code and data memory.
class TestROM : public Device {
  public:
    static constexpr std::size_t SIZE = 512;
    std::array<uint8_t, SIZE> mem{};

    explicit TestROM(SNES *snes) : Device(snes) {}

    TickResult tick(time_master_delta_t budget) override { return {budget, TickStopReason::BudgetExhausted}; }
    void onEvent(const SchedulerEvent &) override {}

    uint8_t readRegister(uint32_t offset) override { return mem[offset % SIZE]; }
    void writeRegister(uint32_t offset, uint8_t data) override { mem[offset % SIZE] = data; }
};

// Map a TestROM so that bank 0x00, pages 0x80–0x81 (addresses $008000–$0081FF)
// back the 512-byte ROM window.  CPU PC starts at $8000.
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

    // Write a byte sequence into the ROM starting at offset from $8000.
    void loadAt(uint16_t offset, std::initializer_list<uint8_t> bytes) {
        std::size_t i = offset & 0x1FFu;
        for (uint8_t b : bytes) {
            rom.mem[i++ & 0x1FFu] = b;
        }
    }
};

// ---------------------------------------------------------------------------
// Construction and initial state
// ---------------------------------------------------------------------------

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

    // Starts in emulation mode with I flag set.
    REQUIRE(r.P.E == true);
    REQUIRE(r.P.M == true);
    REQUIRE(r.P.X == true);
    REQUIRE(r.P.I == true);

    REQUIRE(cpu.getMicroOpIndex() == 0);
}

// ---------------------------------------------------------------------------
// NOP (0xEA) — 2 cycles
// ---------------------------------------------------------------------------

TEST_CASE("NOP: tick(2) consumes exactly 2 cycles", "[cpu]") {
    TestFixture f;
    f.loadAt(0, {0xEA}); // NOP at $8000

    TickResult r = f.cpu.tick(2);
    REQUIRE(r.completed_cycles == 2);
    REQUIRE(r.reason == TickStopReason::BudgetExhausted);
    REQUIRE(f.cpu.regs().PC == 0x8001);
    REQUIRE(f.cpu.getTime() == 2);
}

TEST_CASE("NOP: tick(1) consumes 1 cycle and stops mid-instruction", "[cpu]") {
    TestFixture f;
    f.loadAt(0, {0xEA}); // NOP at $8000

    TickResult r = f.cpu.tick(1);
    REQUIRE(r.completed_cycles == 1);
    REQUIRE(r.reason == TickStopReason::BudgetExhausted);
    REQUIRE(f.cpu.regs().PC == 0x8001);
    REQUIRE(f.cpu.getMicroOpIndex() == 1);
    REQUIRE(f.cpu.getTime() == 1);
}

TEST_CASE("NOP: two tick(1) calls equal one tick(2)", "[cpu]") {
    TestFixture f;
    f.loadAt(0, {0xEA, 0xEA}); // NOP NOP at $8000

    (void)f.cpu.tick(1);
    (void)f.cpu.tick(1);

    REQUIRE(f.cpu.getTime() == 2);
    REQUIRE(f.cpu.regs().PC == 0x8001);
}

TEST_CASE("NOP: three consecutive NOPs advance PC by 3", "[cpu]") {
    TestFixture f;
    f.loadAt(0, {0xEA, 0xEA, 0xEA});

    TickResult r = f.cpu.tick(6);
    REQUIRE(r.completed_cycles == 6);
    REQUIRE(f.cpu.regs().PC == 0x8003);
    REQUIRE(f.cpu.getTime() == 6);
}

// ---------------------------------------------------------------------------
// LDA #imm (0xA9) — 2 cycles in 8-bit accumulator mode
// ---------------------------------------------------------------------------

TEST_CASE("LDA #imm: loads immediate byte into A", "[cpu]") {
    TestFixture f;
    f.loadAt(0, {0xA9, 0x42});

    TickResult r = f.cpu.tick(2);
    REQUIRE(r.completed_cycles == 2);
    REQUIRE(static_cast<uint8_t>(f.cpu.regs().A) == 0x42);
    REQUIRE(f.cpu.regs().PC == 0x8002);
    REQUIRE(f.cpu.getTime() == 2);
}

TEST_CASE("LDA #imm: updates N flag for high-bit values", "[cpu]") {
    TestFixture f;
    f.loadAt(0, {0xA9, 0x80});

    (void)f.cpu.tick(2);
    REQUIRE(static_cast<uint8_t>(f.cpu.regs().A) == 0x80);
    REQUIRE(f.cpu.regs().P.N == true);
    REQUIRE(f.cpu.regs().P.Z == false);
}

TEST_CASE("LDA #imm: updates Z flag for zero", "[cpu]") {
    TestFixture f;
    f.loadAt(0, {0xA9, 0x00});

    (void)f.cpu.tick(2);
    REQUIRE(static_cast<uint8_t>(f.cpu.regs().A) == 0x00);
    REQUIRE(f.cpu.regs().P.Z == true);
    REQUIRE(f.cpu.regs().P.N == false);
}

TEST_CASE("LDA #imm: clears N and Z for nonzero, non-high-bit values", "[cpu]") {
    TestFixture f;
    f.loadAt(0, {0xA9, 0x01});

    (void)f.cpu.tick(2);
    REQUIRE(static_cast<uint8_t>(f.cpu.regs().A) == 0x01);
    REQUIRE(f.cpu.regs().P.N == false);
    REQUIRE(f.cpu.regs().P.Z == false);
}

TEST_CASE("LDA #imm: preserves A high byte (B register)", "[cpu]") {
    TestFixture f;
    f.loadAt(0, {0xA9, 0x55});

    (void)f.cpu.tick(2);
    // In 8-bit mode A high byte (the hidden B register) starts as 0 and must stay 0.
    REQUIRE(f.cpu.regs().A == 0x0055);
}

// ---------------------------------------------------------------------------
// Sequence: NOP then LDA
// ---------------------------------------------------------------------------

TEST_CASE("NOP followed by LDA #imm executes correctly in sequence", "[cpu]") {
    TestFixture f;
    f.loadAt(0, {0xEA, 0xA9, 0x7F});

    TickResult r = f.cpu.tick(4);
    REQUIRE(r.completed_cycles == 4);
    REQUIRE(f.cpu.regs().PC == 0x8003);
    REQUIRE(static_cast<uint8_t>(f.cpu.regs().A) == 0x7F);
    REQUIRE(f.cpu.getTime() == 4);
}

// ---------------------------------------------------------------------------
// Unmapped (open-bus) reads
// ---------------------------------------------------------------------------

TEST_CASE("Fetch from unmapped address returns open-bus value and advances PC", "[cpu]") {
    // No pages mapped — every read returns the open-bus value.
    SNES snes;
    CPU cpu(&snes);

    TickResult r = cpu.tick(2);
    REQUIRE(r.completed_cycles == 2);
    REQUIRE(cpu.regs().PC == 0x0001);
    REQUIRE(cpu.getTime() == 2);
}

// ---------------------------------------------------------------------------
// CpuFlags
// ---------------------------------------------------------------------------

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
    f.fromByte(0x00, /*emulation_mode=*/false);
    REQUIRE(f.M == false);
    REQUIRE(f.X == false);

    f.fromByte(0x30, false); // bits 5 and 4
    REQUIRE(f.M == true);
    REQUIRE(f.X == true);
}

TEST_CASE("CpuFlags::fromByte in emulation mode ignores M and X bits", "[cpu]") {
    CpuFlags f{};
    f.M = true;
    f.X = true;
    f.fromByte(0x00, /*emulation_mode=*/true);
    // M and X must remain true in emulation mode.
    REQUIRE(f.M == true);
    REQUIRE(f.X == true);
}
