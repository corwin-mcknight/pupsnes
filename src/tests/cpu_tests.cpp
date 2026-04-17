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

  TickResult Tick(TimeMasterDeltaT budget) override {
    return {budget, TickStopReason::kBudgetExhausted};
  }
  void OnEvent(const SchedulerEvent&) override {}

  uint8_t ReadRegister(uint32_t offset) override { return mem[offset % kSize]; }
  void WriteRegister(uint32_t offset, uint8_t data) override {
    mem[offset % kSize] = data;
  }
};

class ObservedMMIO : public Device {
 public:
  static constexpr std::size_t kSize = 512;
  std::array<uint8_t, kSize> mem{};
  std::vector<TimeMasterT> read_times;

  explicit ObservedMMIO(SNES* snes) : Device(snes) {}

  TickResult Tick(TimeMasterDeltaT budget) override {
    return {budget, TickStopReason::kBudgetExhausted};
  }
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
    snes.system_bus->MapPage(
        {0x00, 0x80, rom.GetDeviceId(), 0x000, PageDeviceKind::kMemory, 8});
    snes.system_bus->MapPage(
        {0x00, 0x81, rom.GetDeviceId(), 0x100, PageDeviceKind::kMemory, 8});

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
    snes.system_bus->MapPage({0x00, 0x80, program.GetDeviceId(), 0x000,
                              PageDeviceKind::kSameClockMmio, 8});
    snes.system_bus->MapPage({0x00, 0x81, program.GetDeviceId(), 0x100,
                              PageDeviceKind::kSameClockMmio, 8});

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
    snes.system_bus->MapPage({0x00, 0x80, async_target.GetDeviceId(), 0x000,
                              PageDeviceKind::kCrossClockMmio, 8});

    auto r = cpu.GetRegs();
    r.PC = 0x8000;
    cpu.SetRegs(r);
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
    SetResetVector(0x8000);
    wram.MapSystemBus(*snes.system_bus);
    SyncCartridge();
  }

  void SetResetVector(uint16_t address) {
    rom[0x7FFCU] = static_cast<uint8_t>(address & 0x00FFU);
    rom[0x7FFDU] = static_cast<uint8_t>(address >> 8U);
  }

  void SetRomByte(std::size_t offset, uint8_t value) { rom[offset] = value; }

  void SyncCartridge() {
    cartridge.LoadLoRom(rom);
    cartridge.MapLoRom(*snes.system_bus);
  }
};

TEST_CASE("CPU registers with SNES on construction", "[cpu]") {
  SNES snes;
  CPU cpu(&snes);

  REQUIRE(cpu.GetDeviceId() != static_cast<DeviceIdT>(-1));
  REQUIRE(snes.GetDevice(cpu.GetDeviceId()) == &cpu);
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

TEST_CASE("CPU reset fetches the reset vector through cartridge mapping",
          "[cpu]") {
  ResetFixture f;

  f.cpu.Reset();

  REQUIRE(f.cpu.GetRegs().PBR == 0);
  REQUIRE(f.cpu.GetRegs().PC == 0x8000);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FF);
  REQUIRE(f.cpu.GetRegs().P.E == true);
  REQUIRE(f.cpu.GetRegs().P.M == true);
  REQUIRE(f.cpu.GetRegs().P.X == true);
  REQUIRE(f.cpu.GetRegs().P.I == true);
  REQUIRE(f.cpu.GetMicroOpIndex() == 0);
  REQUIRE(f.cpu.GetTime() == 0);
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

TEST_CASE("BNE not taken falls through without the guarded branch cycle",
          "[cpu]") {
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

TEST_CASE(
    "Direct CPU tick advances execution state but not committed device time",
    "[cpu]") {
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

TEST_CASE("NOP tick slices still compose correctly without local_time mutation",
          "[cpu]") {
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

TEST_CASE("LDA immediate updates A and flags through direct tick", "[cpu]") {
  TestFixture f;
  f.LoadAt(0, {0xA9, 0x80});

  TickResult r = f.cpu.Tick(2);
  REQUIRE(r.completed_cycles == 2);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x80);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
  REQUIRE(f.cpu.GetTime() == 0);
}

TEST_CASE("LDX immediate updates X and flags through direct tick", "[cpu]") {
  TestFixture f;
  f.LoadAt(0, {0xA2, 0x80});

  TickResult r = f.cpu.Tick(2);
  REQUIRE(r.completed_cycles == 2);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().X) == 0x80);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
  REQUIRE(f.cpu.GetTime() == 0);
}

TEST_CASE(
    "Fetch from unmapped address returns open-bus value and faults on the "
    "unimplemented opcode",
    "[cpu]") {
  SNES snes;
  CPU cpu(&snes);

  TickResult r = cpu.Tick(2);
  REQUIRE(r.completed_cycles == 1);
  REQUIRE(r.reason == TickStopReason::kFaulted);
  REQUIRE(cpu.GetRegs().PC == 0x0001);
  REQUIRE(cpu.GetTime() == 0);
  REQUIRE(cpu.GetFault().has_value());
  REQUIRE(cpu.GetFault()->opcode == 0xFF);
  REQUIRE(cpu.GetFault()->opcode_address == 0x000000U);
}

TEST_CASE(
    "Consecutive same-tick bus accesses use increasing absolute timestamps",
    "[cpu]") {
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
  f.snes.scheduler->ScheduleEvent(2, &f.rom, SchedulerPhase::kWakeSample,
                                  EventType::kDeviceBoundary);
  f.snes.scheduler->ScheduleEvent(4, &f.rom, SchedulerPhase::kWakeSample,
                                  EventType::kDeviceBoundary);

  f.snes.scheduler->Step();
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetTime() == 2);
  REQUIRE(SchedulerTestAccess::HasPendingRun(*f.snes.scheduler,
                                             f.cpu.GetDeviceId()));
  REQUIRE(SchedulerTestAccess::PendingRunTime(*f.snes.scheduler,
                                              f.cpu.GetDeviceId()) == 2);

  f.snes.scheduler->Step();
  REQUIRE(f.snes.GetMasterTime() == 2);

  f.snes.scheduler->Step();
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x42);
  REQUIRE(f.cpu.GetTime() == 4);
  REQUIRE(SchedulerTestAccess::PendingRunTime(*f.snes.scheduler,
                                              f.cpu.GetDeviceId()) == 4);
}

TEST_CASE("Direct CPU tick faults on unimplemented opcode fetch", "[cpu]") {
  TestFixture f;
  f.LoadAt(0, {0x00});

  TickResult r = f.cpu.Tick(2);

  REQUIRE(r.completed_cycles == 1);
  REQUIRE(r.reason == TickStopReason::kFaulted);
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetTime() == 0);
  REQUIRE(f.cpu.GetFault().has_value());
  REQUIRE(f.cpu.GetFault()->opcode == 0x00);
  REQUIRE(f.cpu.GetFault()->opcode_address == 0x008000U);
}

TEST_CASE("Scheduler-driven CPU faults are terminal and do not reschedule",
          "[cpu]") {
  TestFixture f;
  f.LoadAt(0, {0x00});

  f.snes.scheduler->ScheduleDeviceRun(&f.cpu, 0);
  f.snes.scheduler->Step();

  REQUIRE(f.cpu.GetTime() == 1);
  REQUIRE(f.cpu.GetFault().has_value());
  REQUIRE(f.cpu.GetFault()->opcode == 0x00);
  REQUIRE_FALSE(SchedulerTestAccess::HasPendingRun(*f.snes.scheduler,
                                                   f.cpu.GetDeviceId()));
}

TEST_CASE(
    "Cross-clock token completion wakes a blocked CPU through authoritative "
    "run scheduling",
    "[cpu]") {
  AsyncProgramFixture f;

  f.snes.scheduler->ScheduleDeviceRun(&f.cpu, 0);

  f.snes.scheduler->Step();
  const auto blocked_token =
      SchedulerTestAccess::BlockedToken(*f.snes.scheduler, f.cpu.GetDeviceId());
  REQUIRE(blocked_token != 0);
  REQUIRE_FALSE(SchedulerTestAccess::HasPendingRun(*f.snes.scheduler,
                                                   f.cpu.GetDeviceId()));
  REQUIRE(f.cpu.GetTime() == 0);

  f.snes.scheduler->Step();
  REQUIRE(f.snes.GetMasterTime() == 8);
  REQUIRE(SchedulerTestAccess::HasPendingRun(*f.snes.scheduler,
                                             f.cpu.GetDeviceId()));
  REQUIRE(SchedulerTestAccess::PendingRunTime(*f.snes.scheduler,
                                              f.cpu.GetDeviceId()) == 8);
}

TEST_CASE(
    "Same-clock scheduler-driven fetches keep absolute bus timestamps and CPU "
    "time aligned",
    "[cpu]") {
  MMIOProgramFixture f;
  f.LoadAt(0, {0xA9, 0x7F});

  f.snes.scheduler->ScheduleDeviceRun(&f.cpu, 100);
  f.snes.scheduler->ScheduleEvent(102, &f.program, SchedulerPhase::kWakeSample,
                                  EventType::kDeviceBoundary);

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

TEST_CASE("CpuFlags::FromByte in emulation mode ignores M and X bits",
          "[cpu]") {
  CpuFlags f{};
  f.M = true;
  f.X = true;
  f.FromByte(0x00, true);
  REQUIRE(f.M == true);
  REQUIRE(f.X == true);
}
