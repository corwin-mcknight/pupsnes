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

using namespace pupsnes;  // NOLINT(google-build-using-namespace)

class TestROM : public Device {
 public:
  static constexpr std::size_t kSize = 512;
  std::array<uint8_t, kSize> mem{};

  explicit TestROM(SNES* snes) : Device(snes) {}

  TickResult Tick(TimeMasterDeltaT budget) override { return {budget, TickStopReason::kBudgetExhausted}; }
  void OnEvent(const SchedulerEvent&) override {}

  uint8_t ReadRegister(uint32_t offset) override { return mem[offset % kSize]; }
  void WriteRegister(uint32_t offset, uint8_t data) override { mem[offset % kSize] = data; }
};

class ObservedMMIO : public Device {
 public:
  static constexpr std::size_t kSize = 512;
  std::array<uint8_t, kSize> mem{};
  std::vector<TimeMasterT> read_times;

  explicit ObservedMMIO(SNES* snes) : Device(snes) {}

  TickResult Tick(TimeMasterDeltaT budget) override { return {budget, TickStopReason::kBudgetExhausted}; }
  void OnEvent(const SchedulerEvent&) override {}

  uint8_t ReadRegister(uint32_t offset) override {
    read_times.push_back(GetTime());
    return mem[offset % kSize];
  }
};

struct TestFixture {
  SNES snes;
  TestROM rom{&snes};
  CPU cpu{&snes};

  TestFixture() {
    snes.system_bus->MapPage({0x00, 0x80, rom.GetDeviceId(), 0x000, PageDeviceKind::kMemory, 8});
    snes.system_bus->MapPage({0x00, 0x81, rom.GetDeviceId(), 0x100, PageDeviceKind::kMemory, 8});

    auto r = cpu.GetRegs();
    r.PC = 0x8000;
    cpu.SetRegs(r);
  }

  void LoadAt(uint16_t offset, std::initializer_list<uint8_t> bytes) {
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
    snes.system_bus->MapPage({0x00, 0x80, program.GetDeviceId(), 0x000, PageDeviceKind::kSameClockMmio, 8});
    snes.system_bus->MapPage({0x00, 0x81, program.GetDeviceId(), 0x100, PageDeviceKind::kSameClockMmio, 8});

    auto r = cpu.GetRegs();
    r.PC = 0x8000;
    cpu.SetRegs(r);
  }

  void LoadAt(uint16_t offset, std::initializer_list<uint8_t> bytes) {
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
    snes.system_bus->MapPage({0x00, 0x80, async_target.GetDeviceId(), 0x000, PageDeviceKind::kCrossClockMmio, 8});

    auto r = cpu.GetRegs();
    r.PC = 0x8000;
    cpu.SetRegs(r);
  }
};

using RegPtr = uint16_t CPU::Regs::*;

static void SetDataBank(CPU& cpu, uint8_t dbr) {
  auto regs = cpu.GetRegs();
  regs.DBR = dbr;
  cpu.SetRegs(regs);
}

static void SetAccumulator16(CPU& cpu, uint16_t value) {
  auto regs = cpu.GetRegs();
  regs.P.E = false;
  regs.P.M = false;
  regs.A = value;
  cpu.SetRegs(regs);
}

static void SetIndex16X(CPU& cpu, uint16_t value) {
  auto regs = cpu.GetRegs();
  regs.P.E = false;
  regs.P.X = false;
  regs.X = value;
  cpu.SetRegs(regs);
}

static void SetIndex16Y(CPU& cpu, uint16_t value) {
  auto regs = cpu.GetRegs();
  regs.P.E = false;
  regs.P.X = false;
  regs.Y = value;
  cpu.SetRegs(regs);
}

struct ResetFixture {
  SNES snes;
  Cartridge& cartridge;
  WRAM& wram;
  CPU& cpu;
  std::array<uint8_t, Cartridge::kLoROMWindowSize> rom{};

  ResetFixture() : cartridge(snes.GetCartridge()), wram(snes.GetWram()), cpu(snes.GetCpu()) {
    rom.fill(0xEA);
    SetResetVector(0x8000);
    SyncCartridge();
  }

  void SetResetVector(uint16_t address) {
    rom[0x7FFCU] = static_cast<uint8_t>(address & 0x00FFU);
    rom[0x7FFDU] = static_cast<uint8_t>(address >> 8U);
  }

  void SetRomByte(std::size_t offset, uint8_t value) { rom[offset] = value; }

  void SyncCartridge() { snes.LoadLoRom(rom); }
};

TEST_CASE("CPU registers with SNES on construction", "[cpu]") {
  SNES snes;
  CPU cpu(&snes);

  REQUIRE(cpu.GetDeviceId() != static_cast<DeviceIdT>(-1));
  REQUIRE(snes.GetDevice(cpu.GetDeviceId()) == &cpu);
}

TEST_CASE("SNES owns the core machine devices", "[cpu]") {
  SNES snes;

  REQUIRE(snes.GetDevice(snes.GetCpu().GetDeviceId()) == &snes.GetCpu());
  REQUIRE(snes.GetDevice(snes.GetCartridge().GetDeviceId()) == &snes.GetCartridge());
  REQUIRE(snes.GetDevice(snes.GetWram().GetDeviceId()) == &snes.GetWram());
}

TEST_CASE("CPU initial register state", "[cpu]") {
  SNES snes;
  CPU cpu(&snes);

  auto r = cpu.GetRegs();
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
  REQUIRE(cpu.GetMicroOpIndex() == 0);
}

TEST_CASE("CPU reset fetches the reset vector through cartridge mapping", "[cpu]") {
  ResetFixture f;

  f.snes.Reset();

  REQUIRE(f.snes.GetCpu().GetRegs().PBR == 0);
  REQUIRE(f.snes.GetCpu().GetRegs().PC == 0x8000);
  REQUIRE(f.snes.GetCpu().GetRegs().SP == 0x01FF);
  REQUIRE(f.snes.GetCpu().GetRegs().P.E == true);
  REQUIRE(f.snes.GetCpu().GetRegs().P.M == true);
  REQUIRE(f.snes.GetCpu().GetRegs().P.X == true);
  REQUIRE(f.snes.GetCpu().GetRegs().P.I == true);
  REQUIRE(f.snes.GetCpu().GetMicroOpIndex() == 0);
  REQUIRE(f.snes.GetCpu().GetTime() == 0);
}

TEST_CASE(
    "CPU reset clears in-flight execution state and resumes from the reset "
    "vector",
    "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xA9);
  f.SetRomByte(0x0001U, 0x11);
  f.SyncCartridge();

  auto regs = f.cpu.GetRegs();
  regs.A = 0x00FF;
  regs.PC = 0x8000;
  regs.PBR = 0x00;
  regs.P.D = true;
  regs.P.C = true;
  f.cpu.SetRegs(regs);
  (void)f.cpu.Tick(1);
  REQUIRE(f.cpu.GetMicroOpIndex() == 1);

  f.cpu.Reset();

  REQUIRE(f.cpu.GetRegs().PC == 0x8000);
  REQUIRE(f.cpu.GetRegs().PBR == 0);
  REQUIRE(f.cpu.GetRegs().A == 0);
  REQUIRE(f.cpu.GetRegs().P.D == false);
  REQUIRE(f.cpu.GetRegs().P.C == false);
  REQUIRE(f.cpu.GetMicroOpIndex() == 0);
}

TEST_CASE("CPU executes from the cartridge after reset", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xA9);
  f.SetRomByte(0x0001U, 0x42);
  f.SyncCartridge();

  f.cpu.Reset();
  TickResult r = f.cpu.Tick(2);

  REQUIRE(r.completed_cycles == 2);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8002);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x42);
}

TEST_CASE("BRA branches relative to the post-operand PC", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x80);
  f.SetRomByte(0x0001U, 0x02);
  f.SetRomByte(0x0004U, 0xA9);
  f.SetRomByte(0x0005U, 0x7F);
  f.SyncCartridge();

  f.cpu.Reset();
  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8006);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x7F);
}

TEST_CASE("BRA supports negative displacements for tight loops", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x80);
  f.SetRomByte(0x0001U, 0xFE);
  f.SyncCartridge();

  f.cpu.Reset();
  TickResult r = f.cpu.Tick(6);

  REQUIRE(r.completed_cycles == 6);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8000);
}

TEST_CASE("BNE not taken falls through without the guarded branch cycle", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xD0);
  f.SetRomByte(0x0001U, 0x02);
  f.SetRomByte(0x0002U, 0xEA);
  f.SyncCartridge();

  f.cpu.Reset();
  auto regs = f.cpu.GetRegs();
  regs.P.Z = true;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(2);

  REQUIRE(r.completed_cycles == 2);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8002);
  REQUIRE(f.cpu.GetMicroOpIndex() == 0);
}

TEST_CASE("BNE taken executes the guarded branch cycle", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xD0);
  f.SetRomByte(0x0001U, 0x02);
  f.SetRomByte(0x0004U, 0xA9);
  f.SetRomByte(0x0005U, 0x7F);
  f.SyncCartridge();

  f.cpu.Reset();
  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8006);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x7F);
}

TEST_CASE("BNE supports negative displacements for tight loops", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xD0);
  f.SetRomByte(0x0001U, 0xFE);
  f.SyncCartridge();

  f.cpu.Reset();
  TickResult r = f.cpu.Tick(6);

  REQUIRE(r.completed_cycles == 6);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8000);
}

TEST_CASE("BRA adds a penalty cycle when a taken branch crosses a page in emulation mode", "[cpu]") {
  ResetFixture f;
  f.SetResetVector(0x80FD);
  f.SetRomByte(0x00FDU, 0x80);
  f.SetRomByte(0x00FEU, 0x04);
  f.SyncCartridge();

  f.cpu.Reset();
  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8103);
}

TEST_CASE("BRA page-cross penalty does not fire in native mode", "[cpu]") {
  ResetFixture f;
  f.SetResetVector(0x80FD);
  f.SetRomByte(0x00FDU, 0x80);
  f.SetRomByte(0x00FEU, 0x04);
  f.SyncCartridge();

  f.cpu.Reset();
  auto regs = f.cpu.GetRegs();
  regs.P.E = false;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(3);

  REQUIRE(r.completed_cycles == 3);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8103);
}

TEST_CASE("BNE taken with page cross in emulation mode consumes the penalty cycle", "[cpu]") {
  ResetFixture f;
  f.SetResetVector(0x80FD);
  f.SetRomByte(0x00FDU, 0xD0);
  f.SetRomByte(0x00FEU, 0x04);
  f.SyncCartridge();

  f.cpu.Reset();
  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8103);
}

TEST_CASE("STA long writes accumulator low byte to mapped WRAM", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xA9);
  f.SetRomByte(0x0001U, 0x5A);
  f.SetRomByte(0x0002U, 0x8F);
  f.SetRomByte(0x0003U, 0x00);
  f.SetRomByte(0x0004U, 0x00);
  f.SetRomByte(0x0005U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  TickResult r = f.cpu.Tick(7);

  REQUIRE(r.completed_cycles == 7);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x5A);
  REQUIRE(f.cpu.GetRegs().PC == 0x8006);
  REQUIRE(f.wram.Peek(0x0000) == 0x5A);
}

TEST_CASE("STA absolute uses DBR and writes accumulator low byte to WRAM", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x8D);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  auto regs = f.cpu.GetRegs();
  regs.A = 0x005A;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(f.wram.Peek(0x0000) == 0x5A);
}

TEST_CASE("STX absolute uses DBR and writes X low byte to WRAM", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x8E);
  f.SetRomByte(0x0001U, 0x01);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0034;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(f.wram.Peek(0x0001) == 0x34);
}

TEST_CASE("STY absolute uses DBR and writes Y low byte to WRAM", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x8C);
  f.SetRomByte(0x0001U, 0x02);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);
  auto regs = f.cpu.GetRegs();
  regs.Y = 0x0078;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(f.wram.Peek(0x0002) == 0x78);
}

TEST_CASE("STA long writes both accumulator bytes when M is clear", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x8F);
  f.SetRomByte(0x0001U, 0x00);
  f.SetRomByte(0x0002U, 0x00);
  f.SetRomByte(0x0003U, 0x7E);
  f.SyncCartridge();

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0xBEEF);

  TickResult r = f.cpu.Tick(6);

  REQUIRE(r.completed_cycles == 6);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8004);
  REQUIRE(f.wram.Peek(0x0000) == 0xEF);
  REQUIRE(f.wram.Peek(0x0001) == 0xBE);
}

TEST_CASE("STX absolute writes both index bytes when X is clear", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x8E);
  f.SetRomByte(0x0001U, 0x10);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);
  SetIndex16X(f.cpu, 0x1234);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(f.wram.Peek(0x0010) == 0x34);
  REQUIRE(f.wram.Peek(0x0011) == 0x12);
}

TEST_CASE("STY absolute writes both index bytes when X is clear", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x8C);
  f.SetRomByte(0x0001U, 0x20);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);
  SetIndex16Y(f.cpu, 0xABCD);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(f.wram.Peek(0x0020) == 0xCD);
  REQUIRE(f.wram.Peek(0x0021) == 0xAB);
}

TEST_CASE("STA absolute writes both accumulator bytes when M is clear", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x8D);
  f.SetRomByte(0x0001U, 0x30);
  f.SetRomByte(0x0002U, 0x00);
  f.SyncCartridge();

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);
  SetAccumulator16(f.cpu, 0xCAFE);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(f.wram.Peek(0x0030) == 0xFE);
  REQUIRE(f.wram.Peek(0x0031) == 0xCA);
}

TEST_CASE("STA absolute 16-bit high-byte write carries into the next bank at $FFFF", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x8D);
  f.SetRomByte(0x0001U, 0xFF);
  f.SetRomByte(0x0002U, 0xFF);
  f.SyncCartridge();

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);
  SetAccumulator16(f.cpu, 0xBEEF);

  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(f.wram.Peek(0xFFFF) == 0xEF);
  REQUIRE(f.wram.Peek(0x10000) == 0xBE);
}

TEST_CASE("PHA 8-bit pushes accumulator low byte and decrements SP", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xA9);
  f.SetRomByte(0x0001U, 0x42);
  f.SetRomByte(0x0002U, 0x48);
  f.SyncCartridge();

  f.cpu.Reset();
  TickResult r = f.cpu.Tick(5);

  REQUIRE(r.completed_cycles == 5);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FE);
  REQUIRE(f.wram.Peek(0x01FF) == 0x42);
}

TEST_CASE("PHA 8-bit in emulation mode wraps SP across page 1", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x48);
  f.SyncCartridge();

  f.cpu.Reset();
  auto regs = f.cpu.GetRegs();
  regs.A = 0x007A;
  regs.SP = 0x0100;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(3);

  REQUIRE(r.completed_cycles == 3);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FF);
  REQUIRE(f.wram.Peek(0x0100) == 0x7A);
}

TEST_CASE("PHA 16-bit pushes both accumulator bytes when M is clear", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x48);
  f.SyncCartridge();

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0xBEEF);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FD);
  REQUIRE(f.wram.Peek(0x01FF) == 0xBE);
  REQUIRE(f.wram.Peek(0x01FE) == 0xEF);
}

TEST_CASE("PHB pushes data bank register and decrements SP", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x8B);
  f.SyncCartridge();

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.Tick(3);

  REQUIRE(r.completed_cycles == 3);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetRegs().DBR == 0x7E);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FE);
  REQUIRE(f.wram.Peek(0x01FF) == 0x7E);
}

TEST_CASE("PLB pulls data bank register from stack and updates DBR and flags", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xAB);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x01FF, 0x42);
  auto regs = f.cpu.GetRegs();
  regs.SP = 0x01FE;
  regs.DBR = 0x00;
  regs.P.N = true;
  regs.P.Z = true;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetRegs().DBR == 0x42);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FF);
  REQUIRE(f.cpu.GetRegs().P.N == false);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("PLB sets Z when pulled value is zero", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xAB);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x01FF, 0x00);
  auto regs = f.cpu.GetRegs();
  regs.SP = 0x01FE;
  regs.DBR = 0x7E;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(f.cpu.GetRegs().DBR == 0x00);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}

TEST_CASE("PLB sets N when pulled value has bit 7 set", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xAB);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x01FF, 0x80);
  auto regs = f.cpu.GetRegs();
  regs.SP = 0x01FE;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(f.cpu.GetRegs().DBR == 0x80);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("PLB in emulation mode wraps SP across page 1", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xAB);
  f.SyncCartridge();

  f.cpu.Reset();
  f.wram.WriteRegister(0x0100, 0x33);
  auto regs = f.cpu.GetRegs();
  regs.SP = 0x01FF;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(4);

  REQUIRE(r.completed_cycles == 4);
  REQUIRE(f.cpu.GetRegs().DBR == 0x33);
  REQUIRE(f.cpu.GetRegs().SP == 0x0100);
}

TEST_CASE("PHB followed by PLB restores DBR", "[cpu]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x8B);
  f.SetRomByte(0x0001U, 0xAB);
  f.SyncCartridge();

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.Tick(7);

  REQUIRE(r.completed_cycles == 7);
  REQUIRE(r.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8002);
  REQUIRE(f.cpu.GetRegs().DBR == 0x7E);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FF);
}

TEST_CASE("Direct CPU tick advances execution state but not committed device time", "[cpu]") {
  TestFixture f;
  f.LoadAt(0, {0xEA, 0xA9, 0x42});

  TickResult r1 = f.cpu.Tick(2);
  REQUIRE(r1.completed_cycles == 2);
  REQUIRE(r1.reason == TickStopReason::kBudgetExhausted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetTime() == 0);

  TickResult r2 = f.cpu.Tick(2);
  REQUIRE(r2.completed_cycles == 2);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x42);
  REQUIRE(f.cpu.GetTime() == 0);
}

TEST_CASE("NOP tick slices still compose correctly without local_time mutation", "[cpu]") {
  TestFixture f;
  f.LoadAt(0, {0xEA, 0xEA});

  (void)f.cpu.Tick(1);
  REQUIRE(f.cpu.GetMicroOpIndex() == 1);
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetTime() == 0);

  (void)f.cpu.Tick(1);
  REQUIRE(f.cpu.GetMicroOpIndex() == 0);
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetTime() == 0);
}

static void CheckImm8Load(uint8_t opcode, RegPtr reg) {
  TestFixture f;
  f.LoadAt(0, {opcode, 0x80});
  TickResult r = f.cpu.Tick(2);
  auto regs = f.cpu.GetRegs();
  REQUIRE(r.completed_cycles == 2);
  REQUIRE(static_cast<uint8_t>(regs.*reg) == 0x80);
  REQUIRE(regs.P.N == true);
  REQUIRE(regs.P.Z == false);
  REQUIRE(f.cpu.GetTime() == 0);
}

TEST_CASE("LDA immediate updates A and flags through direct tick", "[cpu]") { CheckImm8Load(0xA9, &CPU::Regs::A); }
TEST_CASE("LDX immediate updates X and flags through direct tick", "[cpu]") { CheckImm8Load(0xA2, &CPU::Regs::X); }
TEST_CASE("LDY immediate updates Y and flags through direct tick", "[cpu]") { CheckImm8Load(0xA0, &CPU::Regs::Y); }

static void CheckImm16Load(uint8_t opcode, uint8_t lo, uint8_t hi, uint16_t expected, bool clear_m, RegPtr reg,
                           bool expect_n) {
  TestFixture f;
  f.LoadAt(0, {opcode, lo, hi});
  auto regs = f.cpu.GetRegs();
  regs.P.E = false;
  (clear_m ? regs.P.M : regs.P.X) = false;
  f.cpu.SetRegs(regs);
  TickResult r = f.cpu.Tick(3);
  auto out = f.cpu.GetRegs();
  REQUIRE(r.completed_cycles == 3);
  REQUIRE(out.*reg == expected);
  REQUIRE(out.PC == 0x8003);
  REQUIRE(out.P.N == expect_n);
  REQUIRE(out.P.Z == false);
}

TEST_CASE("LDA immediate uses 16-bit width when M is clear", "[cpu]") {
  CheckImm16Load(0xA9, 0xAB, 0xCD, 0xCDAB, true, &CPU::Regs::A, true);
}
TEST_CASE("LDX immediate uses 16-bit width when X is clear", "[cpu]") {
  CheckImm16Load(0xA2, 0x34, 0x12, 0x1234, false, &CPU::Regs::X, false);
}
TEST_CASE("LDY immediate uses 16-bit width when X is clear", "[cpu]") {
  CheckImm16Load(0xA0, 0x00, 0x80, 0x8000, false, &CPU::Regs::Y, true);
}

TEST_CASE(
    "Fetch from unmapped address returns open-bus value and faults on the "
    "unimplemented opcode",
    "[cpu]") {
  SNES snes;
  CPU cpu(&snes);
  auto regs = cpu.GetRegs();
  regs.PBR = 0x40;
  cpu.SetRegs(regs);

  TickResult r = cpu.Tick(2);
  REQUIRE(r.completed_cycles == 1);
  REQUIRE(r.reason == TickStopReason::kFaulted);
  REQUIRE(cpu.GetRegs().PC == 0x0001);
  REQUIRE(cpu.GetTime() == 0);
  const auto& fault = cpu.GetFault();
  REQUIRE(fault.has_value());
  if (!fault) return;
  REQUIRE(fault->opcode == 0xFF);
  REQUIRE(fault->opcode_address == 0x400000U);
}

TEST_CASE("Consecutive same-tick bus accesses use increasing absolute timestamps", "[cpu]") {
  MMIOProgramFixture f;
  f.LoadAt(0, {0xA9, 0x42});
  f.cpu.AdvanceLocalTime(100);

  TickResult r = f.cpu.Tick(2);

  REQUIRE(r.completed_cycles == 2);
  REQUIRE(f.program.read_times == std::vector<TimeMasterT>{100, 101});
  REQUIRE(f.cpu.GetTime() == 100);
}

TEST_CASE(
    "Scheduler-driven CPU execution commits time and reschedules after "
    "BudgetExhausted",
    "[cpu]") {
  TestFixture f;
  f.LoadAt(0, {0xEA, 0xA9, 0x42});

  f.snes.scheduler->ScheduleDeviceRun(&f.cpu, 0);
  f.snes.scheduler->ScheduleEvent(2, &f.rom, SchedulerPhase::kWakeSample, EventType::kDeviceBoundary);
  f.snes.scheduler->ScheduleEvent(4, &f.rom, SchedulerPhase::kWakeSample, EventType::kDeviceBoundary);

  f.snes.scheduler->Step();
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetTime() == 2);
  REQUIRE(SchedulerTestAccess::HasPendingRun(*f.snes.scheduler, f.cpu.GetDeviceId()));
  REQUIRE(SchedulerTestAccess::PendingRunTime(*f.snes.scheduler, f.cpu.GetDeviceId()) == 2);

  f.snes.scheduler->Step();
  REQUIRE(f.snes.GetMasterTime() == 2);

  f.snes.scheduler->Step();
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x42);
  REQUIRE(f.cpu.GetTime() == 4);
  REQUIRE(SchedulerTestAccess::PendingRunTime(*f.snes.scheduler, f.cpu.GetDeviceId()) == 4);
}

TEST_CASE("Direct CPU tick faults on unimplemented opcode fetch", "[cpu]") {
  TestFixture f;
  f.LoadAt(0, {0x00});

  TickResult r = f.cpu.Tick(2);

  REQUIRE(r.completed_cycles == 1);
  REQUIRE(r.reason == TickStopReason::kFaulted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetTime() == 0);
  const auto& fault = f.cpu.GetFault();
  REQUIRE(fault.has_value());
  if (!fault) return;
  REQUIRE(fault->opcode == 0x00);
  REQUIRE(fault->opcode_address == 0x008000U);
}

TEST_CASE("Scheduler-driven CPU faults are terminal and do not reschedule", "[cpu]") {
  TestFixture f;
  f.LoadAt(0, {0x00});

  f.snes.scheduler->ScheduleDeviceRun(&f.cpu, 0);
  f.snes.scheduler->Step();

  REQUIRE(f.cpu.GetTime() == 1);
  const auto& fault = f.cpu.GetFault();
  REQUIRE(fault.has_value());
  if (!fault) return;
  REQUIRE(fault->opcode == 0x00);
  REQUIRE_FALSE(SchedulerTestAccess::HasPendingRun(*f.snes.scheduler, f.cpu.GetDeviceId()));
}

TEST_CASE(
    "Cross-clock token completion wakes a blocked CPU through authoritative "
    "run scheduling",
    "[cpu]") {
  AsyncProgramFixture f;

  f.snes.scheduler->ScheduleDeviceRun(&f.cpu, 0);

  f.snes.scheduler->Step();
  const auto blocked_token = SchedulerTestAccess::BlockedToken(*f.snes.scheduler, f.cpu.GetDeviceId());
  REQUIRE(blocked_token != 0);
  REQUIRE_FALSE(SchedulerTestAccess::HasPendingRun(*f.snes.scheduler, f.cpu.GetDeviceId()));
  REQUIRE(f.cpu.GetTime() == 0);

  f.snes.scheduler->Step();
  REQUIRE(f.snes.GetMasterTime() == 8);
  REQUIRE(SchedulerTestAccess::HasPendingRun(*f.snes.scheduler, f.cpu.GetDeviceId()));
  REQUIRE(SchedulerTestAccess::PendingRunTime(*f.snes.scheduler, f.cpu.GetDeviceId()) == 8);
}

TEST_CASE(
    "Same-clock scheduler-driven fetches keep absolute bus timestamps and CPU "
    "time aligned",
    "[cpu]") {
  MMIOProgramFixture f;
  f.LoadAt(0, {0xA9, 0x7F});

  f.snes.scheduler->ScheduleDeviceRun(&f.cpu, 100);
  f.snes.scheduler->ScheduleEvent(102, &f.program, SchedulerPhase::kWakeSample, EventType::kDeviceBoundary);

  f.snes.scheduler->Step();

  REQUIRE(f.program.read_times == std::vector<TimeMasterT>{100, 101});
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x7F);
  REQUIRE(f.cpu.GetTime() == 102);
}

TEST_CASE("CpuFlags::ToByte encodes all flags correctly", "[cpu]") {
  CpuFlags f{};
  f.N = true;
  f.V = true;
  f.M = true;
  f.X = true;
  f.D = true;
  f.I = true;
  f.Z = true;
  f.C = true;
  REQUIRE(f.ToByte() == 0xFF);
}

TEST_CASE("CpuFlags::ToByte with no flags set returns 0", "[cpu]") {
  CpuFlags f{};
  f.N = false;
  f.V = false;
  f.M = false;
  f.X = false;
  f.D = false;
  f.I = false;
  f.Z = false;
  f.C = false;
  REQUIRE(f.ToByte() == 0x00);
}

TEST_CASE("CpuFlags::FromByte in native mode sets M and X from byte", "[cpu]") {
  CpuFlags f{};
  f.FromByte(0x00, false);
  REQUIRE(f.M == false);
  REQUIRE(f.X == false);

  f.FromByte(0x30, false);
  REQUIRE(f.M == true);
  REQUIRE(f.X == true);
}

TEST_CASE("CpuFlags::FromByte in emulation mode ignores M and X bits", "[cpu]") {
  CpuFlags f{};
  f.M = true;
  f.X = true;
  f.FromByte(0x00, true);
  REQUIRE(f.M == true);
  REQUIRE(f.X == true);
}

TEST_CASE("INX 8-bit increments X low byte and updates N/Z", "[cpu][inc]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xE8);
  f.SyncCartridge();

  f.cpu.Reset();
  auto regs = f.cpu.GetRegs();
  regs.X = 0x007F;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.Tick(2);

  REQUIRE(r.completed_cycles == 2);
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetRegs().X == 0x0080);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("INX 8-bit wraps to zero and sets Z", "[cpu][inc]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xE8);
  f.SyncCartridge();

  f.cpu.Reset();
  auto regs = f.cpu.GetRegs();
  regs.X = 0x12FF;  // high byte preserved in 8-bit mode
  f.cpu.SetRegs(regs);

  (void)f.cpu.Tick(2);

  REQUIRE(f.cpu.GetRegs().X == 0x1200);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}

TEST_CASE("INX 16-bit increments full register across page", "[cpu][inc]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xE8);
  f.SyncCartridge();

  f.cpu.Reset();
  SetIndex16X(f.cpu, 0x7FFF);

  (void)f.cpu.Tick(2);

  REQUIRE(f.cpu.GetRegs().X == 0x8000);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("INY 8-bit increments Y and sets Z on wrap", "[cpu][inc]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xC8);
  f.SyncCartridge();

  f.cpu.Reset();
  auto regs = f.cpu.GetRegs();
  regs.Y = 0x00FF;
  f.cpu.SetRegs(regs);

  (void)f.cpu.Tick(2);

  REQUIRE(f.cpu.GetRegs().Y == 0x0000);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

TEST_CASE("INY 16-bit wraps at $FFFF to 0", "[cpu][inc]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xC8);
  f.SyncCartridge();

  f.cpu.Reset();
  SetIndex16Y(f.cpu, 0xFFFF);

  (void)f.cpu.Tick(2);

  REQUIRE(f.cpu.GetRegs().Y == 0x0000);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}

TEST_CASE("DEX 8-bit wraps to $FF and sets N", "[cpu][dec]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xCA);
  f.SyncCartridge();

  f.cpu.Reset();
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0000;
  f.cpu.SetRegs(regs);

  (void)f.cpu.Tick(2);

  REQUIRE(f.cpu.GetRegs().X == 0x00FF);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("DEX 16-bit decrements full register", "[cpu][dec]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0xCA);
  f.SyncCartridge();

  f.cpu.Reset();
  SetIndex16X(f.cpu, 0x0001);

  (void)f.cpu.Tick(2);

  REQUIRE(f.cpu.GetRegs().X == 0x0000);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}

TEST_CASE("DEY 8-bit decrements Y", "[cpu][dec]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x88);
  f.SyncCartridge();

  f.cpu.Reset();
  auto regs = f.cpu.GetRegs();
  regs.Y = 0x0001;
  f.cpu.SetRegs(regs);

  (void)f.cpu.Tick(2);

  REQUIRE(f.cpu.GetRegs().Y == 0x0000);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

TEST_CASE("INC A 8-bit increments accumulator low byte, preserves high", "[cpu][inc]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x1A);
  f.SyncCartridge();

  f.cpu.Reset();
  auto regs = f.cpu.GetRegs();
  regs.A = 0xAB7F;  // high byte preserved in 8-bit mode
  f.cpu.SetRegs(regs);

  (void)f.cpu.Tick(2);

  REQUIRE(f.cpu.GetRegs().A == 0xAB80);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("INC A 16-bit increments full accumulator", "[cpu][inc]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x1A);
  f.SyncCartridge();

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0xFFFF);

  (void)f.cpu.Tick(2);

  REQUIRE(f.cpu.GetRegs().A == 0x0000);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}

TEST_CASE("DEC A 8-bit decrements accumulator low byte", "[cpu][dec]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x3A);
  f.SyncCartridge();

  f.cpu.Reset();
  auto regs = f.cpu.GetRegs();
  regs.A = 0x1200;
  f.cpu.SetRegs(regs);

  (void)f.cpu.Tick(2);

  REQUIRE(f.cpu.GetRegs().A == 0x12FF);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("DEC A 16-bit decrements full accumulator", "[cpu][dec]") {
  ResetFixture f;
  f.SetRomByte(0x0000U, 0x3A);
  f.SyncCartridge();

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0x0001);

  (void)f.cpu.Tick(2);

  REQUIRE(f.cpu.GetRegs().A == 0x0000);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

TEST_CASE("DRAM refresh stalls the CPU for 40 cycles mid-scanline", "[cpu][refresh]") {
  ResetFixture f;
  // Fill the first scanline worth of program with NOPs (2 cycles each).
  for (std::size_t i = 0; i < 800; ++i) {
    f.SetRomByte(i, 0xEA);
  }
  f.SyncCartridge();
  f.cpu.Reset();

  const uint64_t retired_before = f.cpu.GetRetiredInstructionCount();

  // Run exactly one full scanline's worth of master cycles. Without refresh
  // the CPU would retire scanline/2 = 682 NOPs; refresh steals 40 of those
  // cycles, so only (1364-40)/2 = 662 NOPs should retire.
  TickResult r = f.cpu.Tick(kMasterCyclesPerScanline);
  const uint64_t retired = f.cpu.GetRetiredInstructionCount() - retired_before;

  REQUIRE(r.completed_cycles == kMasterCyclesPerScanline);
  REQUIRE(retired == (kMasterCyclesPerScanline - kDramRefreshDurationCycles) / 2);
  REQUIRE(f.cpu.GetRefreshStallWindows() == 1);
  REQUIRE(f.cpu.GetRefreshStallCycles() == kDramRefreshDurationCycles);
}

TEST_CASE("DRAM refresh fires once per scanline", "[cpu][refresh]") {
  ResetFixture f;
  for (std::size_t i = 0; i < 2000; ++i) {
    f.SetRomByte(i, 0xEA);
  }
  f.SyncCartridge();
  f.cpu.Reset();

  const uint64_t retired_before = f.cpu.GetRetiredInstructionCount();

  // Three scanlines → three refresh windows → 120 stall cycles total.
  TickResult r = f.cpu.Tick(3 * kMasterCyclesPerScanline);
  const uint64_t retired = f.cpu.GetRetiredInstructionCount() - retired_before;

  REQUIRE(r.completed_cycles == 3 * kMasterCyclesPerScanline);
  REQUIRE(retired == (3 * kMasterCyclesPerScanline - 3 * kDramRefreshDurationCycles) / 2);
  REQUIRE(f.cpu.GetRefreshStallWindows() == 3);
  REQUIRE(f.cpu.GetRefreshStallCycles() == 3 * kDramRefreshDurationCycles);
}

TEST_CASE("DRAM refresh does not fire before kDramRefreshStartCycle", "[cpu][refresh]") {
  ResetFixture f;
  for (std::size_t i = 0; i < 600; ++i) {
    f.SetRomByte(i, 0xEA);
  }
  f.SyncCartridge();
  f.cpu.Reset();

  const uint64_t retired_before = f.cpu.GetRetiredInstructionCount();

  // Run up to just before the refresh window.
  TickResult r = f.cpu.Tick(kDramRefreshStartCycle);
  const uint64_t retired = f.cpu.GetRetiredInstructionCount() - retired_before;

  REQUIRE(r.completed_cycles == kDramRefreshStartCycle);
  REQUIRE(retired == kDramRefreshStartCycle / 2);
}
