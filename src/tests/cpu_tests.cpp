#include <catch2/catch_test_macros.hpp>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"

#include <array>
#include <cstdint>
#include <cstring>

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
// back the 512-byte ROM window.  The CPU's PC starts at 0x0000 by default, but
// tests that want to execute from ROM set PC = 0x8000.
struct TestFixture {
    SNES snes;
    TestROM rom{&snes};
    CPU cpu{&snes};

    TestFixture() {
        // Map 512-byte ROM window at bank $00, pages $80–$81.
        snes.system_bus->mapPage({0x00, 0x80, rom.getDeviceId(), 0x000, PageDeviceKind::Memory, 8});
        snes.system_bus->mapPage({0x00, 0x81, rom.getDeviceId(), 0x100, PageDeviceKind::Memory, 8});
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

    REQUIRE(cpu.getPC() == 0x0000);
    REQUIRE(cpu.getA() == 0);
    REQUIRE(cpu.getX() == 0);
    REQUIRE(cpu.getY() == 0);
    REQUIRE(cpu.getSP() == 0x01FF);
    REQUIRE(cpu.getPBR() == 0);
    REQUIRE(cpu.getDBR() == 0);
    REQUIRE(cpu.getDP() == 0);

    // Starts in emulation mode with I flag set.
    REQUIRE(cpu.getFlags().E == true);
    REQUIRE(cpu.getFlags().M == true);
    REQUIRE(cpu.getFlags().X == true);
    REQUIRE(cpu.getFlags().I == true);

    REQUIRE(cpu.getMicroOpIndex() == 0);
}

// ---------------------------------------------------------------------------
// NOP (0xEA) — 2 cycles
// ---------------------------------------------------------------------------

TEST_CASE("NOP: tick(2) consumes exactly 2 cycles", "[cpu]") {
    TestFixture f;
    f.loadAt(0, {0xEA});       // NOP at $8000
    f.cpu.advanceLocalTime(0); // already 0
    // Set PC to ROM start by constructing the CPU with defaults and mapping.
    // We need to manually set PC — expose it via a helper or derive from address.
    // For this test we use unmapped $0000 which returns open-bus 0xFF (SBC variant),
    // but we want NOP.  Use the ROM by setting PC = 0x8000 via a tiny workaround:
    // We can't call setPC yet — add it to CPU, or test via behavior.
    //
    // Instead, write NOP at $0000 region by mapping another page at bank $00, page $00.
    SNES snes2;
    TestROM rom2(&snes2);
    CPU cpu2(&snes2);
    snes2.system_bus->mapPage({0x00, 0x00, rom2.getDeviceId(), 0, PageDeviceKind::Memory, 8});
    rom2.mem[0] = 0xEA; // NOP at $0000

    TickResult r = cpu2.tick(2);
    REQUIRE(r.completed_cycles == 2);
    REQUIRE(r.reason == TickStopReason::BudgetExhausted);
    REQUIRE(cpu2.getPC() == 0x0001); // Opcode fetch incremented PC
    REQUIRE(cpu2.getTime() == 2);
}

TEST_CASE("NOP: tick(1) consumes 1 cycle and stops mid-instruction", "[cpu]") {
    SNES snes;
    TestROM rom(&snes);
    CPU cpu(&snes);
    snes.system_bus->mapPage({0x00, 0x00, rom.getDeviceId(), 0, PageDeviceKind::Memory, 8});
    rom.mem[0] = 0xEA; // NOP

    TickResult r = cpu.tick(1);
    REQUIRE(r.completed_cycles == 1);
    REQUIRE(r.reason == TickStopReason::BudgetExhausted);
    REQUIRE(cpu.getPC() == 0x0001);      // Opcode fetch done
    REQUIRE(cpu.getMicroOpIndex() == 1); // In the middle of NOP (at the internal cycle)
    REQUIRE(cpu.getTime() == 1);
}

TEST_CASE("NOP: two tick(1) calls equal one tick(2)", "[cpu]") {
    SNES snes;
    TestROM rom(&snes);
    CPU cpu(&snes);
    snes.system_bus->mapPage({0x00, 0x00, rom.getDeviceId(), 0, PageDeviceKind::Memory, 8});
    rom.mem[0] = 0xEA; // NOP at $0000
    rom.mem[1] = 0xEA; // NOP at $0001

    (void)cpu.tick(1); // cycle 1 of NOP: opcode fetch
    (void)cpu.tick(1); // cycle 2 of NOP: internal

    REQUIRE(cpu.getTime() == 2);
    REQUIRE(cpu.getPC() == 0x0001);
}

TEST_CASE("NOP: three consecutive NOPs advance PC by 3", "[cpu]") {
    SNES snes;
    TestROM rom(&snes);
    CPU cpu(&snes);
    snes.system_bus->mapPage({0x00, 0x00, rom.getDeviceId(), 0, PageDeviceKind::Memory, 8});
    rom.mem[0] = 0xEA;
    rom.mem[1] = 0xEA;
    rom.mem[2] = 0xEA;

    // 3 NOPs × 2 cycles = 6 cycles
    TickResult r = cpu.tick(6);
    REQUIRE(r.completed_cycles == 6);
    REQUIRE(cpu.getPC() == 0x0003);
    REQUIRE(cpu.getTime() == 6);
}

// ---------------------------------------------------------------------------
// LDA #imm (0xA9) — 2 cycles in 8-bit accumulator mode
// ---------------------------------------------------------------------------

TEST_CASE("LDA #imm: loads immediate byte into A", "[cpu]") {
    SNES snes;
    TestROM rom(&snes);
    CPU cpu(&snes);
    snes.system_bus->mapPage({0x00, 0x00, rom.getDeviceId(), 0, PageDeviceKind::Memory, 8});
    rom.mem[0] = 0xA9; // LDA #imm
    rom.mem[1] = 0x42; // immediate value

    TickResult r = cpu.tick(2);
    REQUIRE(r.completed_cycles == 2);
    REQUIRE(cpu.getA() == 0x42);
    REQUIRE(cpu.getPC() == 0x0002); // consumed opcode + immediate
    REQUIRE(cpu.getTime() == 2);
}

TEST_CASE("LDA #imm: updates N flag for high-bit values", "[cpu]") {
    SNES snes;
    TestROM rom(&snes);
    CPU cpu(&snes);
    snes.system_bus->mapPage({0x00, 0x00, rom.getDeviceId(), 0, PageDeviceKind::Memory, 8});
    rom.mem[0] = 0xA9;
    rom.mem[1] = 0x80; // bit 7 set

    (void)cpu.tick(2);
    REQUIRE(cpu.getA() == 0x80);
    REQUIRE(cpu.getFlags().N == true);
    REQUIRE(cpu.getFlags().Z == false);
}

TEST_CASE("LDA #imm: updates Z flag for zero", "[cpu]") {
    SNES snes;
    TestROM rom(&snes);
    CPU cpu(&snes);
    snes.system_bus->mapPage({0x00, 0x00, rom.getDeviceId(), 0, PageDeviceKind::Memory, 8});
    rom.mem[0] = 0xA9;
    rom.mem[1] = 0x00;

    (void)cpu.tick(2);
    REQUIRE(cpu.getA() == 0x00);
    REQUIRE(cpu.getFlags().Z == true);
    REQUIRE(cpu.getFlags().N == false);
}

TEST_CASE("LDA #imm: clears N and Z for nonzero, non-high-bit values", "[cpu]") {
    SNES snes;
    TestROM rom(&snes);
    CPU cpu(&snes);
    snes.system_bus->mapPage({0x00, 0x00, rom.getDeviceId(), 0, PageDeviceKind::Memory, 8});
    rom.mem[0] = 0xA9;
    rom.mem[1] = 0x01;

    (void)cpu.tick(2);
    REQUIRE(cpu.getA() == 0x01);
    REQUIRE(cpu.getFlags().N == false);
    REQUIRE(cpu.getFlags().Z == false);
}

TEST_CASE("LDA #imm: preserves A high byte (B register)", "[cpu]") {
    SNES snes;
    TestROM rom(&snes);
    CPU cpu(&snes);
    snes.system_bus->mapPage({0x00, 0x00, rom.getDeviceId(), 0, PageDeviceKind::Memory, 8});
    rom.mem[0] = 0xA9;
    rom.mem[1] = 0x55;

    (void)cpu.tick(2);
    // In 8-bit mode A high byte (the hidden B register) starts as 0 and must stay 0.
    REQUIRE(cpu.getAFull() == 0x0055);
}

// ---------------------------------------------------------------------------
// Sequence: NOP then LDA
// ---------------------------------------------------------------------------

TEST_CASE("NOP followed by LDA #imm executes correctly in sequence", "[cpu]") {
    SNES snes;
    TestROM rom(&snes);
    CPU cpu(&snes);
    snes.system_bus->mapPage({0x00, 0x00, rom.getDeviceId(), 0, PageDeviceKind::Memory, 8});
    rom.mem[0] = 0xEA; // NOP  (2 cycles)
    rom.mem[1] = 0xA9; // LDA #imm (2 cycles)
    rom.mem[2] = 0x7F; // immediate

    // Execute both instructions in one tick call.
    TickResult r = cpu.tick(4);
    REQUIRE(r.completed_cycles == 4);
    REQUIRE(cpu.getPC() == 0x0003);
    REQUIRE(cpu.getA() == 0x7F);
    REQUIRE(cpu.getTime() == 4);
}

// ---------------------------------------------------------------------------
// Unmapped (open-bus) reads
// ---------------------------------------------------------------------------

TEST_CASE("Fetch from unmapped address returns 0xFF and advances PC", "[cpu]") {
    // No pages mapped — every read returns open-bus 0xFF (SBC (dp,S),Y on 65C816).
    // This test just verifies the CPU doesn't crash and advances time.
    SNES snes;
    CPU cpu(&snes);

    TickResult r = cpu.tick(2);
    REQUIRE(r.completed_cycles == 2);
    REQUIRE(cpu.getPC() == 0x0001);
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
