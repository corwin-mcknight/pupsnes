#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "cpu_test_fixture.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/cartridge.h"
#include "pupsnes/hw/device.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"
#include "pupsnes/hw/wram.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)

class TestROM : public Device {
 public:
  static constexpr std::size_t kSize = 512;
  std::array<uint8_t, kSize> mem{};

  explicit TestROM(SNES* snes) : Device(snes) {}

  MmioReadResult ReadRegister(uint32_t offset, TimeMasterT /*current_time*/) override {
    return {mem[offset % kSize], 0xFFU};
  }
  void WriteRegister(uint32_t offset, uint8_t data, TimeMasterT /*current_time*/) override {
    mem[offset % kSize] = data;
  }
};

class ObservedMMIO : public Device {
 public:
  static constexpr std::size_t kSize = 512;
  std::array<uint8_t, kSize> mem{};
  std::vector<TimeMasterT> read_times;

  explicit ObservedMMIO(SNES* snes) : Device(snes) {}

  void CatchUpTo(TimeMasterT target) override { local_time_ = target; }

  MmioReadResult ReadRegister(uint32_t offset, TimeMasterT /*current_time*/) override {
    read_times.push_back(GetTime());
    return {mem[offset % kSize], 0xFFU};
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

using RegPtr = uint16_t CPU::Regs::*;

using pupsnes::test::ResetFixture;
using pupsnes::test::SetAccumulator16;
using pupsnes::test::SetDataBank;
using pupsnes::test::SetIndex16X;
using pupsnes::test::SetIndex16Y;

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
  f.LoadInstruction({0xA9, 0x11});

  f.ModifyRegs([](auto& r) {
    r.A = 0x00FF;
    r.PC = 0x8000;
    r.PBR = 0x00;
    r.P.D = true;
    r.P.C = true;
  });
  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 8);
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
  f.LoadInstruction({0xA9, 0x42});

  f.cpu.Reset();
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 16);

  REQUIRE(r.completed_cycles == 16);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
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
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8006);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x7F);
}

TEST_CASE("BRA supports negative displacements for tight loops", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x80, 0xFE});

  f.cpu.Reset();
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 44);

  REQUIRE(r.completed_cycles == 44);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8000);
}

TEST_CASE("BNE not taken in emulation with DP-low nonzero does not spuriously add cycle", "[cpu]") {
  // Regression: bit 4 of the rule-eval truth table aliases kBranchPageCrossed
  // and kDirectPageLowNonzero. If FetchOpcode seeds dp_low_nonzero for branch
  // opcodes, branches that don't cross a page still trigger the emulation
  // page-cross penalty when (DP & 0xFF) != 0.
  ResetFixture f;
  f.LoadInstruction({0xD0, 0x02});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.P.Z = true;   // BNE not taken
    r.P.E = true;   // Emulation mode
    r.DP = 0x0055;  // DL nonzero would spuriously set bit 4 without the fix
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 16);

  REQUIRE(r.completed_cycles == 16);
  REQUIRE(f.cpu.GetRegs().PC == 0x8002);
}

TEST_CASE("BNE not taken falls through without the guarded branch cycle", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0xD0, 0x02, 0xEA});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) { r.P.Z = true; });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 16);

  REQUIRE(r.completed_cycles == 16);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
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
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8006);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x7F);
}

TEST_CASE("BNE supports negative displacements for tight loops", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0xD0, 0xFE});

  f.cpu.Reset();
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 44);

  REQUIRE(r.completed_cycles == 44);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8000);
}

TEST_CASE("BRA adds a penalty cycle when a taken branch crosses a page in emulation mode", "[cpu]") {
  ResetFixture f;
  f.SetResetVector(0x80FD);
  f.SetRomByte(0x00FDU, 0x80);
  f.SetRomByte(0x00FEU, 0x04);
  f.SyncCartridge();

  f.cpu.Reset();
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 28);

  REQUIRE(r.completed_cycles == 28);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8103);
}

TEST_CASE("BRA page-cross penalty does not fire in native mode", "[cpu]") {
  ResetFixture f;
  f.SetResetVector(0x80FD);
  f.SetRomByte(0x00FDU, 0x80);
  f.SetRomByte(0x00FEU, 0x04);
  f.SyncCartridge();

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) { r.P.E = false; });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 22);

  REQUIRE(r.completed_cycles == 22);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8103);
}

TEST_CASE("BNE taken with page cross in emulation mode consumes the penalty cycle", "[cpu]") {
  ResetFixture f;
  f.SetResetVector(0x80FD);
  f.SetRomByte(0x00FDU, 0xD0);
  f.SetRomByte(0x00FEU, 0x04);
  f.SyncCartridge();

  f.cpu.Reset();
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 28);

  REQUIRE(r.completed_cycles == 28);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8103);
}

TEST_CASE("STA long writes accumulator low byte to mapped WRAM", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0xA9, 0x5A, 0x8F, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 56);

  REQUIRE(r.completed_cycles == 56);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x5A);
  REQUIRE(f.cpu.GetRegs().PC == 0x8006);
  REQUIRE(f.wram.Peek(0x0000) == 0x5A);
}

TEST_CASE("STA absolute uses DBR and writes accumulator low byte to WRAM", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x8D, 0x00, 0x00});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.A = 0x005A;
    r.DBR = 0x7E;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(f.wram.Peek(0x0000) == 0x5A);
}

TEST_CASE("STX absolute uses DBR and writes X low byte to WRAM", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x8E, 0x01, 0x00});

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);
  f.ModifyRegs([](auto& r) { r.X = 0x0034; });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(f.wram.Peek(0x0001) == 0x34);
}

TEST_CASE("STY absolute uses DBR and writes Y low byte to WRAM", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x8C, 0x02, 0x00});

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);
  f.ModifyRegs([](auto& r) { r.Y = 0x0078; });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(f.wram.Peek(0x0002) == 0x78);
}

TEST_CASE("STA long writes both accumulator bytes when M is clear", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x8F, 0x00, 0x00, 0x7E});

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0xBEEF);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8004);
  REQUIRE(f.wram.Peek(0x0000) == 0xEF);
  REQUIRE(f.wram.Peek(0x0001) == 0xBE);
}

TEST_CASE("STX absolute writes both index bytes when X is clear", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x8E, 0x10, 0x00});

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);
  SetIndex16X(f.cpu, 0x1234);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(f.wram.Peek(0x0010) == 0x34);
  REQUIRE(f.wram.Peek(0x0011) == 0x12);
}

TEST_CASE("STY absolute writes both index bytes when X is clear", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x8C, 0x20, 0x00});

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);
  SetIndex16Y(f.cpu, 0xABCD);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(f.wram.Peek(0x0020) == 0xCD);
  REQUIRE(f.wram.Peek(0x0021) == 0xAB);
}

TEST_CASE("STA absolute writes both accumulator bytes when M is clear", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x8D, 0x30, 0x00});

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);
  SetAccumulator16(f.cpu, 0xCAFE);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(f.wram.Peek(0x0030) == 0xFE);
  REQUIRE(f.wram.Peek(0x0031) == 0xCA);
}

TEST_CASE("STA direct page writes accumulator low byte with DP=0 and no DL penalty", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0xA9, 0x5A, 0x85, 0x10});

  f.cpu.Reset();
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);  // LDA#2 + STA dp (3 with M=1,DL=0)

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x5A);
  REQUIRE(f.cpu.GetRegs().PC == 0x8004);
  REQUIRE(f.wram.Peek(0x0010) == 0x5A);
}

TEST_CASE("STA direct page incurs +1 cycle penalty when DP low byte is nonzero", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x85, 0x10});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.A = 0x00A7;
    r.DP = 0x0020;  // DL nonzero → +1 cycle
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 30);

  REQUIRE(r.completed_cycles == 30);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8002);
  REQUIRE(f.wram.Peek(0x0030) == 0xA7);
}

TEST_CASE("STA direct page writes both accumulator bytes when M is clear", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x85, 0x40});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.P.M = false;
    r.A = 0xBEEF;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);  // 4-m+w with m=0,w=0 = 4

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8002);
  REQUIRE(f.wram.Peek(0x0040) == 0xEF);
  REQUIRE(f.wram.Peek(0x0041) == 0xBE);
}

TEST_CASE("LDA direct page loads from bank 0 (DP + offset)", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0xA5, 0x50});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0050, 0x42, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 24);  // 4-m+w with m=1,w=0 = 3

  REQUIRE(r.completed_cycles == 24);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x42);
  REQUIRE(f.cpu.GetRegs().PC == 0x8002);
  REQUIRE(f.cpu.GetRegs().P.N == false);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("LDA direct page loads 16-bit value when M is clear", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0xA5, 0x80});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0080, 0xCD, 0);
  f.wram.WriteRegister(0x0081, 0xAB, 0);
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.P.M = false;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);  // 4-m+w with m=0,w=0 = 4

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().A == 0xABCD);
  REQUIRE(f.cpu.GetRegs().PC == 0x8002);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("LDA direct page incurs DL-nonzero penalty cycle", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0xA5, 0x10});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.DP = 0x0123;  // DL nonzero → +1 cycle
  });
  f.wram.WriteRegister(0x0133, 0x99, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 30);  // 4-m+w = 3 + w(1) = 4

  REQUIRE(r.completed_cycles == 30);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x99);
  REQUIRE(f.cpu.GetRegs().PC == 0x8002);
}

TEST_CASE("LDX/LDY/STX/STY/STZ direct page cover register + zero paths", "[cpu]") {
  ResetFixture f;
  // LDX $10 ; STX $14 ; LDY $11 ; STY $15 ; STZ $12
  f.LoadInstruction({0xA6, 0x10, 0x86, 0x14, 0xA4, 0x11, 0x84, 0x15, 0x64, 0x12});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0010, 0x7A, 0);
  f.wram.WriteRegister(0x0011, 0x2B, 0);
  f.wram.WriteRegister(0x0012, 0xFF, 0);  // STZ should overwrite this

  // Each op is 3 cycles (M=1/X=1, DL=0) = 15 total.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 120);

  REQUIRE(r.completed_cycles == 120);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().X) == 0x7A);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().Y) == 0x2B);
  REQUIRE(f.wram.Peek(0x0014) == 0x7A);
  REQUIRE(f.wram.Peek(0x0015) == 0x2B);
  REQUIRE(f.wram.Peek(0x0012) == 0x00);
  REQUIRE(f.cpu.GetRegs().PC == 0x800A);
}

TEST_CASE("STZ direct page writes zero to both DP bytes when M is clear", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x64, 0x20});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0020, 0xAA, 0);
  f.wram.WriteRegister(0x0021, 0xBB, 0);
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.P.M = false;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.wram.Peek(0x0020) == 0x00);
  REQUIRE(f.wram.Peek(0x0021) == 0x00);
}

TEST_CASE("LDA direct page indexed X reads (DP + offset + X) in bank 0", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0xB5, 0x20});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.X = 0x000A;  // 8-bit X
  });
  f.wram.WriteRegister(0x002A, 0x55, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 30);  // 5-m+w with m=1,w=0 = 4

  REQUIRE(r.completed_cycles == 30);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x55);
  REQUIRE(f.cpu.GetRegs().PC == 0x8002);
}

TEST_CASE("LDX direct page indexed Y respects 16-bit index addition", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0xB6, 0x10});

  f.cpu.Reset();
  SetIndex16Y(f.cpu, 0x0080);  // 16-bit Y so X=0 in P
  f.wram.WriteRegister(0x0090, 0xCD, 0);
  f.wram.WriteRegister(0x0091, 0xAB, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);  // 5-x+w with x=0,w=0 = 5

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().X == 0xABCD);
}

TEST_CASE("STA direct page indexed X incurs DL-nonzero penalty", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x95, 0x10});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.A = 0x003C;
    r.X = 0x0002;
    r.DP = 0x0040;  // DL nonzero → +1 cycle
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 44);  // 5-m+w with m=1,w=1 = 5

  REQUIRE(r.completed_cycles == 44);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.wram.Peek(0x0052) == 0x3C);
}

TEST_CASE("STZ direct page indexed X clears bank 0 byte", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x74, 0x30});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) { r.X = 0x0004; });
  f.wram.WriteRegister(0x0034, 0x77, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.wram.Peek(0x0034) == 0x00);
}

TEST_CASE("STZ absolute writes zero through DBR-banked effective address", "[cpu]") {
  ResetFixture f;
  // STZ $1234
  f.LoadInstruction({0x9C, 0x34, 0x12});

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);
  f.wram.WriteRegister(0x1234, 0xAB, 0);

  // 5-m with m=1 = 4 cycles.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.wram.Peek(0x1234) == 0x00);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
}

TEST_CASE("STZ absolute clears both bytes when M is clear", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x9C, 0x40, 0x00});

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);
  SetAccumulator16(f.cpu, 0x1234);  // also clears M
  f.wram.WriteRegister(0x0040, 0xAA, 0);
  f.wram.WriteRegister(0x0041, 0xBB, 0);

  // 5-m with m=0 = 5 cycles.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.wram.Peek(0x0040) == 0x00);
  REQUIRE(f.wram.Peek(0x0041) == 0x00);
}

TEST_CASE("STZ absolute indexed X adds X into the DBR-banked effective address", "[cpu]") {
  ResetFixture f;
  // STZ $1200,X
  f.LoadInstruction({0x9E, 0x00, 0x12});

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);
  f.ModifyRegs([](auto& r) {
    r.X = 0x0034;  // 8-bit X
  });
  f.wram.WriteRegister(0x1234, 0x99, 0);

  // 6-m with m=1 = 5 cycles.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.wram.Peek(0x1234) == 0x00);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
}

TEST_CASE("STZ absolute indexed X carries across the bank boundary", "[cpu]") {
  ResetFixture f;
  // STZ $FFFF,X  with X=1  -> effective bank 0x7F:$0000
  f.LoadInstruction({0x9E, 0xFF, 0xFF});

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);
  SetIndex16X(f.cpu, 0x0001);
  f.wram.WriteRegister(0xFFFF, 0xCC, 0);  // not this one
  // 0x7F:$0000 = WRAM linear offset 0x10000; poke via bank 0x7F mapping
  f.ModifyRegs([](auto& r) {
    r.P.M = true;  // 8-bit store
  });

  // Prime WRAM $10000 (bank 0x7F:$0000) with nonzero so we can see the clear.
  f.wram.WriteRegister(0x10000, 0xDD, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);  // 6-m with m=1 = 5

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.wram.Peek(0xFFFF) == 0xCC);
  REQUIRE(f.wram.Peek(0x10000) == 0x00);
}

TEST_CASE("LDA stack-relative reads bank-0 (SP + offset)", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0xA3, 0x04});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) { r.SP = 0x01F0; });
  f.wram.WriteRegister(0x01F4, 0x66, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);  // 5-m at m=1 = 4

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x66);
}

TEST_CASE("STA stack-relative writes to bank-0 (SP + offset)", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x83, 0x03});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.SP = 0x0200;
    r.A = 0x0088;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(f.wram.Peek(0x0203) == 0x88);
}

TEST_CASE("STA absolute indexed X writes to DBR:(abs + X)", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x9D, 0x00, 0x00});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.DBR = 0x7E;
    r.X = 0x0020;
    r.A = 0x0044;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);  // 6-m at m=1 = 5

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.wram.Peek(0x0020) == 0x44);
}

TEST_CASE("STA absolute indexed X can cross bank boundary", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x9D, 0xFE, 0xFF});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.DBR = 0x7E;
    r.X = 0x0003;
    r.A = 0x005A;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  // DBR=$7E, addr = $7EFFFE + 3 = $7F0001 → WRAM offset 0x10001
  REQUIRE(f.wram.Peek(0x10001) == 0x5A);
}

TEST_CASE("STZ absolute indexed X clears a bank-0 byte", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x9E, 0x40, 0x00});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.DBR = 0x7E;
    r.X = 0x0004;
  });
  f.wram.WriteRegister(0x0044, 0x99, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.wram.Peek(0x0044) == 0x00);
}

TEST_CASE("LDA direct indirect reads through pointer at DBR:(high:low)", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0xB2, 0x10});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) { r.DBR = 0x7E; });
  f.wram.WriteRegister(0x0010, 0x34, 0);
  f.wram.WriteRegister(0x0011, 0x12, 0);
  f.wram.WriteRegister(0x1234, 0x99, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x99);
}

TEST_CASE("LDA direct indirect reads 16 bits when M is clear", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0xB2, 0x20});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.P.M = false;
    r.DBR = 0x7E;
  });
  f.wram.WriteRegister(0x0020, 0x00, 0);
  f.wram.WriteRegister(0x0021, 0x20, 0);
  f.wram.WriteRegister(0x2000, 0xCD, 0);
  f.wram.WriteRegister(0x2001, 0xAB, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(f.cpu.GetRegs().A == 0xABCD);
}

TEST_CASE("STA direct indirect writes through pointer", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x92, 0x30});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.A = 0x0044;
    r.DBR = 0x7E;
  });
  f.wram.WriteRegister(0x0030, 0x00, 0);
  f.wram.WriteRegister(0x0031, 0x40, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(f.wram.Peek(0x4000) == 0x44);
}

TEST_CASE("LDA direct indirect long reads through 24-bit pointer", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0xA7, 0x40});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x10, 0);
  f.wram.WriteRegister(0x0041, 0x30, 0);
  f.wram.WriteRegister(0x0042, 0x7E, 0);
  f.wram.WriteRegister(0x3010, 0x11, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x11);
}

TEST_CASE("STA direct indirect long writes through 24-bit pointer", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x87, 0x50});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) { r.A = 0x0077; });
  f.wram.WriteRegister(0x0050, 0x00, 0);
  f.wram.WriteRegister(0x0051, 0x50, 0);
  f.wram.WriteRegister(0x0052, 0x7E, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(f.wram.Peek(0x5000) == 0x77);
}

TEST_CASE("LDA direct indirect with DP-nonzero adds DL penalty cycle", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0xB2, 0x10});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.DP = 0x0080;  // DL nonzero → +1 cycle
    r.DBR = 0x7E;
  });
  f.wram.WriteRegister(0x0090, 0x00, 0);
  f.wram.WriteRegister(0x0091, 0x60, 0);
  f.wram.WriteRegister(0x6000, 0x55, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x55);
}

TEST_CASE("STA absolute 16-bit high-byte write carries into the next bank at $FFFF", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x8D, 0xFF, 0xFF});

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);
  SetAccumulator16(f.cpu, 0xBEEF);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(f.wram.Peek(0xFFFF) == 0xEF);
  REQUIRE(f.wram.Peek(0x10000) == 0xBE);
}

TEST_CASE("PHA 8-bit pushes accumulator low byte and decrements SP", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0xA9, 0x42, 0x48});

  f.cpu.Reset();
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FE);
  REQUIRE(f.wram.Peek(0x01FF) == 0x42);
}

TEST_CASE("PHA 8-bit in emulation mode wraps SP across page 1", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x48});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.A = 0x007A;
    r.SP = 0x0100;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 30);

  REQUIRE(r.completed_cycles == 30);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FF);
  REQUIRE(f.wram.Peek(0x0100) == 0x7A);
}

TEST_CASE("PHA 16-bit pushes both accumulator bytes when M is clear", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x48});

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0xBEEF);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FD);
  REQUIRE(f.wram.Peek(0x01FF) == 0xBE);
  REQUIRE(f.wram.Peek(0x01FE) == 0xEF);
}

TEST_CASE("PHB pushes data bank register and decrements SP", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x8B});

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 22);

  REQUIRE(r.completed_cycles == 22);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetRegs().DBR == 0x7E);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FE);
  REQUIRE(f.wram.Peek(0x01FF) == 0x7E);
}

// PLB-after-Reset completes exactly at master_time + 28; the helper uses that
// precise window so PC is always at 0x8001 (immediately after PLB) — no chance
// of slipping into the next opcode fetch.
static void CheckPlbFlagPermutation(uint8_t stack_value, uint8_t initial_dbr, bool initial_n, bool initial_z,
                                    uint8_t expected_dbr, bool expected_n, bool expected_z) {
  ResetFixture f;
  f.LoadInstruction({0xAB});

  f.cpu.Reset();
  f.wram.WriteRegister(0x01FF, stack_value, 0);
  f.ModifyRegs([&](auto& r) {
    r.SP = 0x01FE;
    r.DBR = initial_dbr;
    r.P.N = initial_n;
    r.P.Z = initial_z;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 28);

  REQUIRE(r.completed_cycles == 28);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetRegs().DBR == expected_dbr);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FF);
  REQUIRE(f.cpu.GetRegs().P.N == expected_n);
  REQUIRE(f.cpu.GetRegs().P.Z == expected_z);
}

TEST_CASE("PLB pulls data bank register from stack and updates DBR and flags", "[cpu]") {
  CheckPlbFlagPermutation(0x42, 0x00, true, true, 0x42, false, false);
}

TEST_CASE("PLB sets Z when pulled value is zero", "[cpu]") {
  CheckPlbFlagPermutation(0x00, 0x7E, false, false, 0x00, false, true);
}

TEST_CASE("PLB sets N when pulled value has bit 7 set", "[cpu]") {
  CheckPlbFlagPermutation(0x80, 0x00, false, false, 0x80, true, false);
}

TEST_CASE("PLB in emulation mode wraps SP across page 1", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0xAB});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0100, 0x33, 0);
  f.ModifyRegs([](auto& r) { r.SP = 0x01FF; });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 36);

  REQUIRE(r.completed_cycles == 36);
  REQUIRE(f.cpu.GetRegs().DBR == 0x33);
  REQUIRE(f.cpu.GetRegs().SP == 0x0100);
}

TEST_CASE("PHB followed by PLB restores DBR", "[cpu]") {
  ResetFixture f;
  f.LoadInstruction({0x8B, 0xAB});

  f.cpu.Reset();
  SetDataBank(f.cpu, 0x7E);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 50);

  REQUIRE(r.completed_cycles == 50);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8002);
  REQUIRE(f.cpu.GetRegs().DBR == 0x7E);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FF);
}

TEST_CASE("Two consecutive TickToTarget calls compose correctly", "[cpu]") {
  TestFixture f;
  f.LoadAt(0, {0xEA, 0xA9, 0x42});

  TickResult r1 = f.cpu.TickToTarget(f.snes.GetMasterTime() + 14);
  REQUIRE(r1.completed_cycles == 14);
  REQUIRE(r1.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetTime() == 14);

  TickResult r2 = f.cpu.TickToTarget(f.snes.GetMasterTime() + 16);
  REQUIRE(r2.completed_cycles == 16);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x42);
  REQUIRE(f.cpu.GetTime() == 30);
}

TEST_CASE("NOP tick slices still compose correctly across boundaries", "[cpu]") {
  TestFixture f;
  f.LoadAt(0, {0xEA, 0xEA});

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 8);
  REQUIRE(f.cpu.GetMicroOpIndex() == 1);
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetTime() == 8);

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 6);
  REQUIRE(f.cpu.GetMicroOpIndex() == 0);
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetTime() == 14);
}

static void CheckImm8Load(uint8_t opcode, RegPtr reg) {
  TestFixture f;
  f.LoadAt(0, {opcode, 0x80});
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 24);
  auto regs = f.cpu.GetRegs();
  REQUIRE(r.completed_cycles == 24);
  REQUIRE(static_cast<uint8_t>(regs.*reg) == 0x80);
  REQUIRE(regs.P.N == true);
  REQUIRE(regs.P.Z == false);
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
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 24);
  auto out = f.cpu.GetRegs();
  REQUIRE(r.completed_cycles == 24);
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

TEST_CASE("Fetch from unmapped address advances PC via open-bus value", "[cpu]") {
  // With no pages mapped, the opcode fetch returns open-bus (0xFF) and the
  // CPU proceeds with whatever that opcode happens to be. This test used to
  // also assert a fault because 0xFF was unimplemented; SBC long,X (0xFF) is
  // now implemented so the fetch no longer faults — kBudgetExhausted is the
  // natural outcome once the CPU starts consuming operand bytes from open bus.
  SNES snes;
  CPU cpu(&snes);
  auto regs = cpu.GetRegs();
  regs.PBR = 0x40;
  cpu.SetRegs(regs);

  TickResult r = cpu.TickToTarget(snes.GetMasterTime() + 8);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(cpu.GetRegs().PBR == 0x40);
  // The opcode fetch at 0x400000 completed (PC advanced past the opcode byte).
  REQUIRE(cpu.GetRegs().PC == 0x0001);
  REQUIRE_FALSE(cpu.GetFault().has_value());
}

TEST_CASE("Consecutive bus accesses within one TickToTarget use increasing absolute timestamps", "[cpu]") {
  MMIOProgramFixture f;
  f.LoadAt(0, {0xA9, 0x42});

  // Run LDA #$42 — two bus reads: opcode fetch at t=0, immediate at t=8.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 16);

  REQUIRE(r.completed_cycles == 16);
  // Each bus access timestamp equals local_time_ + cycle_time at the moment of
  // the access. The second access's timestamp is the first plus the MMIO page's
  // access_speed (8).
  REQUIRE(f.program.read_times == std::vector<TimeMasterT>{0, 8});
  REQUIRE(f.cpu.GetTime() == 16);
}

TEST_CASE("CPU executes NOP then LDA via two successive TickToTarget slices", "[cpu]") {
  TestFixture f;
  f.LoadAt(0, {0xEA, 0xA9, 0x42});

  // NOP in slow ROM costs 14 master cycles; run exactly that much.
  TickResult r1 = f.cpu.TickToTarget(f.snes.GetMasterTime() + 14);
  REQUIRE(r1.reason == TickStopReason::kReachedTarget);
  REQUIRE(r1.completed_cycles == 14);
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetTime() == 14);

  // LDA #$42 costs 16 more master cycles.
  TickResult r2 = f.cpu.TickToTarget(f.snes.GetMasterTime() + 16);
  REQUIRE(r2.reason == TickStopReason::kReachedTarget);
  REQUIRE(r2.completed_cycles == 16);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x42);
  REQUIRE(f.cpu.GetTime() == 30);
}

TEST_CASE("TickToTarget faults on unimplemented opcode fetch", "[cpu]") {
  TestFixture f;
  f.LoadAt(0, {0x00});

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 8);

  REQUIRE(r.reason == TickStopReason::kFault);
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  const auto& fault = f.cpu.GetFault();
  REQUIRE(fault.has_value());
  if (!fault) return;
  REQUIRE(fault->opcode == 0x00);
  REQUIRE(fault->opcode_address == 0x008000U);
}

TEST_CASE("TickToTarget on unimplemented opcode returns kFault without advancing master time", "[cpu]") {
  TestFixture f;
  f.LoadAt(0, {0x00});

  const TimeMasterT before = f.snes.GetMasterTime();
  TickResult r = f.cpu.TickToTarget(before + 8);

  REQUIRE(r.reason == TickStopReason::kFault);
  const auto& fault = f.cpu.GetFault();
  REQUIRE(fault.has_value());
  if (!fault) return;
  REQUIRE(fault->opcode == 0x00);
}

TEST_CASE("Same-master-time bus accesses within TickToTarget use increasing timestamps", "[cpu]") {
  MMIOProgramFixture f;
  f.LoadAt(0, {0xA9, 0x7F});

  // Set SNES master time to 100 and CPU local time to 100 so bus accesses
  // are timestamped from there. LDA #$7F: opcode at t=100, immediate at t=108.
  f.snes.SetMasterTime(100);
  f.cpu.SetLocalTime(100);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 16);

  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.program.read_times == std::vector<TimeMasterT>{100, 108});
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x7F);
  REQUIRE(f.cpu.GetTime() == 116);
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
  f.LoadInstruction({0xE8});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) { r.X = 0x007F; });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 14);

  REQUIRE(r.completed_cycles == 14);
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetRegs().X == 0x0080);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("INX 8-bit wraps to zero and sets Z", "[cpu][inc]") {
  ResetFixture f;
  f.LoadInstruction({0xE8});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.X = 0x12FF;  // high byte preserved in 8-bit mode
  });

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(f.cpu.GetRegs().X == 0x1200);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}

TEST_CASE("INX 16-bit increments full register across page", "[cpu][inc]") {
  ResetFixture f;
  f.LoadInstruction({0xE8});

  f.cpu.Reset();
  SetIndex16X(f.cpu, 0x7FFF);

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(f.cpu.GetRegs().X == 0x8000);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("INY 8-bit increments Y and sets Z on wrap", "[cpu][inc]") {
  ResetFixture f;
  f.LoadInstruction({0xC8});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) { r.Y = 0x00FF; });

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(f.cpu.GetRegs().Y == 0x0000);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

TEST_CASE("INY 16-bit wraps at $FFFF to 0", "[cpu][inc]") {
  ResetFixture f;
  f.LoadInstruction({0xC8});

  f.cpu.Reset();
  SetIndex16Y(f.cpu, 0xFFFF);

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(f.cpu.GetRegs().Y == 0x0000);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}

TEST_CASE("DEX 8-bit wraps to $FF and sets N", "[cpu][dec]") {
  ResetFixture f;
  f.LoadInstruction({0xCA});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) { r.X = 0x0000; });

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(f.cpu.GetRegs().X == 0x00FF);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("DEX 16-bit decrements full register", "[cpu][dec]") {
  ResetFixture f;
  f.LoadInstruction({0xCA});

  f.cpu.Reset();
  SetIndex16X(f.cpu, 0x0001);

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(f.cpu.GetRegs().X == 0x0000);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}

TEST_CASE("DEY 8-bit decrements Y", "[cpu][dec]") {
  ResetFixture f;
  f.LoadInstruction({0x88});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) { r.Y = 0x0001; });

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(f.cpu.GetRegs().Y == 0x0000);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

TEST_CASE("INC A 8-bit increments accumulator low byte, preserves high", "[cpu][inc]") {
  ResetFixture f;
  f.LoadInstruction({0x1A});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.A = 0xAB7F;  // high byte preserved in 8-bit mode
  });

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(f.cpu.GetRegs().A == 0xAB80);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("INC A 16-bit increments full accumulator", "[cpu][inc]") {
  ResetFixture f;
  f.LoadInstruction({0x1A});

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0xFFFF);

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(f.cpu.GetRegs().A == 0x0000);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}

TEST_CASE("DEC A 8-bit decrements accumulator low byte", "[cpu][dec]") {
  ResetFixture f;
  f.LoadInstruction({0x3A});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) { r.A = 0x1200; });

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(f.cpu.GetRegs().A == 0x12FF);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("DEC A 16-bit decrements full accumulator", "[cpu][dec]") {
  ResetFixture f;
  f.LoadInstruction({0x3A});

  f.cpu.Reset();
  SetAccumulator16(f.cpu, 0x0001);

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(f.cpu.GetRegs().A == 0x0000);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

// ============================================================================
// TickToTarget surface tests
// ============================================================================

TEST_CASE("CPU::TickToTarget stops exactly at target on a NOP stream", "[cpu][unit]") {
  ResetFixture f;
  for (std::size_t i = 0; i < 2000; ++i) f.SetRomByte(i, 0xEA);
  f.SyncCartridge();
  f.cpu.Reset();

  f.snes.SetMasterTime(0);
  TickResult r = f.cpu.TickToTarget(100);
  REQUIRE(r.reason == TickStopReason::kReachedTarget);
  REQUIRE(f.snes.GetMasterTime() <= 100);
  REQUIRE(f.cpu.GetTime() == f.snes.GetMasterTime());
}

TEST_CASE("CPU::TickToTarget returns kRetiredStepTarget after N instructions", "[cpu][unit]") {
  ResetFixture f;
  for (std::size_t i = 0; i < 2000; ++i) f.SetRomByte(i, 0xEA);
  f.SyncCartridge();
  f.cpu.Reset();

  f.cpu.MutableDebuggerContract().step_target = 3;
  f.cpu.MutableDebuggerContract().step_granularity = DebuggerContract::StepGranularity::kInstruction;
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 1'000'000);
  REQUIRE(r.reason == TickStopReason::kRetiredStepTarget);
  REQUIRE(f.cpu.GetRetiredInstructionCount() == 3);
}

TEST_CASE("CPU::TickToTarget with microop granularity yields after one micro-op", "[cpu][unit]") {
  ResetFixture f;
  f.SetRomByte(0, 0xA9);  // LDA #$11
  f.SetRomByte(1, 0x11);
  f.SyncCartridge();
  f.cpu.Reset();

  f.cpu.MutableDebuggerContract().step_target = 1;
  f.cpu.MutableDebuggerContract().step_granularity = DebuggerContract::StepGranularity::kMicroOp;
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 1'000'000);
  REQUIRE(r.reason == TickStopReason::kRetiredStepTarget);
  REQUIRE(f.cpu.GetRetiredInstructionCount() == 0);
}

TEST_CASE("DRAM refresh stalls the CPU for 40 cycles mid-scanline", "[cpu][refresh]") {
  ResetFixture f;
  // Fill the first scanline worth of program with NOPs. A NOP in slow ROM
  // costs 14 master cycles: 8 for the opcode fetch + 6 for the internal
  // cycle (kInternalCpuCycleMaster).
  for (std::size_t i = 0; i < 800; ++i) {
    f.SetRomByte(i, 0xEA);
  }
  f.SyncCartridge();
  f.cpu.Reset();

  const uint64_t retired_before = f.cpu.GetRetiredInstructionCount();

  // Run exactly one full scanline's worth of master cycles. Refresh steals
  // 40 cycles; the remainder (1324) fits 94 complete 14-cycle NOPs with a
  // partial NOP straddling the refresh boundary, so 94 instructions retire.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + kMasterCyclesPerScanline);
  const uint64_t retired = f.cpu.GetRetiredInstructionCount() - retired_before;

  REQUIRE(r.completed_cycles == kMasterCyclesPerScanline);
  REQUIRE(retired == 94);
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

  // Three scanlines → three refresh windows → 120 stall cycles total.
  // With strict-no-overshoot, completed_cycles may be slightly less than the
  // full budget (by up to one internal CPU cycle = 6 mcyc).
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 3 * kMasterCyclesPerScanline);

  REQUIRE(r.completed_cycles >= 3 * kMasterCyclesPerScanline - 6);
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

  // Run up to just before the refresh window. 38 complete NOPs fit in 532
  // cycles; the 39th would cost 14 and exceed the 538-cycle target, so the
  // strict-no-overshoot model stops at 532 (< kDramRefreshStartCycle).
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + kDramRefreshStartCycle);
  const uint64_t retired = f.cpu.GetRetiredInstructionCount() - retired_before;

  REQUIRE(retired == 38);
  REQUIRE(f.cpu.GetRefreshStallWindows() == 0);
  REQUIRE(r.completed_cycles <= kDramRefreshStartCycle);
}

TEST_CASE("CLC clears the carry flag in two cycles", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x18});
  auto regs = f.cpu.GetRegs();
  regs.P.C = true;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 14);

  REQUIRE(r.completed_cycles == 14);
  REQUIRE(f.cpu.GetRegs().P.C == false);
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
}

TEST_CASE("SEC sets the carry flag", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x38});
  auto regs = f.cpu.GetRegs();
  regs.P.C = false;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 22);

  REQUIRE(r.completed_cycles == 22);
  REQUIRE(f.cpu.GetRegs().P.C == true);
}

TEST_CASE("CLI / SEI toggle the interrupt-disable flag", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x58, 0x78});

  REQUIRE(f.cpu.GetRegs().P.I == true);
  TickResult r1 = f.cpu.TickToTarget(f.snes.GetMasterTime() + 14);
  REQUIRE(r1.completed_cycles == 14);
  REQUIRE(f.cpu.GetRegs().P.I == false);

  TickResult r2 = f.cpu.TickToTarget(f.snes.GetMasterTime() + 14);
  REQUIRE(r2.completed_cycles == 14);
  REQUIRE(f.cpu.GetRegs().P.I == true);
}

TEST_CASE("CLD / SED toggle the decimal flag", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0xF8, 0xD8});

  TickResult r1 = f.cpu.TickToTarget(f.snes.GetMasterTime() + 14);
  REQUIRE(r1.completed_cycles == 14);
  REQUIRE(f.cpu.GetRegs().P.D == true);

  TickResult r2 = f.cpu.TickToTarget(f.snes.GetMasterTime() + 14);
  REQUIRE(r2.completed_cycles == 14);
  REQUIRE(f.cpu.GetRegs().P.D == false);
}

TEST_CASE("CLV clears overflow without touching other flags", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0xB8});
  auto regs = f.cpu.GetRegs();
  regs.P.V = true;
  regs.P.N = true;
  regs.P.Z = true;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 22);

  REQUIRE(r.completed_cycles == 22);
  const auto out = f.cpu.GetRegs().P;
  REQUIRE(out.V == false);
  REQUIRE(out.N == true);
  REQUIRE(out.Z == true);
}

TEST_CASE("XCE swaps C and E flags and enforces emulation forcing", "[cpu][opcode]") {
  TestFixture f;
  // Native mode with C=1 — XCE enters emulation mode.
  f.LoadAt(0, {0xFB});
  auto regs = f.cpu.GetRegs();
  regs.P.E = false;
  regs.P.M = false;
  regs.P.X = false;
  regs.P.C = true;
  regs.X = 0x1234;
  regs.Y = 0x5678;
  regs.SP = 0x1FF0;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 22);

  REQUIRE(r.completed_cycles == 22);
  const auto out = f.cpu.GetRegs();
  REQUIRE(out.P.E == true);
  REQUIRE(out.P.C == false);
  REQUIRE(out.P.M == true);
  REQUIRE(out.P.X == true);
  REQUIRE(out.X == 0x0034);
  REQUIRE(out.Y == 0x0078);
  REQUIRE(out.SP == 0x01F0);
}

TEST_CASE("XCE from emulation to native leaves widths as chosen by later REP/SEP", "[cpu][opcode]") {
  TestFixture f;
  // Default after reset is E=1, C=0. XCE -> E=0, C=1. M/X stay 1.
  f.LoadAt(0, {0xFB});

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 22);

  REQUIRE(r.completed_cycles == 22);
  const auto out = f.cpu.GetRegs();
  REQUIRE(out.P.E == false);
  REQUIRE(out.P.C == true);
  REQUIRE(out.P.M == true);
  REQUIRE(out.P.X == true);
}

TEST_CASE("REP in native mode clears the specified P bits", "[cpu][opcode]") {
  TestFixture f;
  // REP #$30 clears M and X.
  f.LoadAt(0, {0xC2, 0x30});
  auto regs = f.cpu.GetRegs();
  regs.P.E = false;
  regs.P.M = true;
  regs.P.X = true;
  regs.P.C = true;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 22);

  REQUIRE(r.completed_cycles == 22);
  const auto out = f.cpu.GetRegs().P;
  REQUIRE(out.M == false);
  REQUIRE(out.X == false);
  REQUIRE(out.C == true);
  REQUIRE(f.cpu.GetRegs().PC == 0x8002);
}

TEST_CASE("SEP in native mode sets the specified P bits", "[cpu][opcode]") {
  TestFixture f;
  // SEP #$21 sets M and C.
  f.LoadAt(0, {0xE2, 0x21});
  auto regs = f.cpu.GetRegs();
  regs.P.E = false;
  regs.P.M = false;
  regs.P.X = false;
  regs.P.C = false;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 30);

  REQUIRE(r.completed_cycles == 30);
  const auto out = f.cpu.GetRegs().P;
  REQUIRE(out.M == true);
  REQUIRE(out.C == true);
  REQUIRE(out.X == false);
}

TEST_CASE("SEP #$10 in native mode zeroes X.H and Y.H when X-flag transitions 0->1", "[cpu][opcode]") {
  // 65C816: setting the X (index) flag forces the high byte of X and Y to $00,
  // matching real-hardware behaviour (Bruce Clark §6.13). Without this, 16-bit
  // index values leak through into 8-bit mode and corrupt subsequent reads.
  TestFixture f;
  f.LoadAt(0, {0xE2, 0x10});  // SEP #$10
  auto regs = f.cpu.GetRegs();
  regs.P.E = false;
  regs.P.M = false;
  regs.P.X = false;  // 16-bit index
  regs.X = 0x1234;
  regs.Y = 0x5678;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 30);

  REQUIRE(r.completed_cycles == 30);
  const auto out = f.cpu.GetRegs();
  REQUIRE(out.P.X == true);
  REQUIRE(out.X == 0x0034);
  REQUIRE(out.Y == 0x0078);
}

TEST_CASE("SEP #$30 in native mode zeroes X.H and Y.H but preserves AH", "[cpu][opcode]") {
  // SEP #$30 sets both M and X. The accumulator's hidden high byte (B) must be
  // preserved; only the index registers truncate.
  TestFixture f;
  f.LoadAt(0, {0xE2, 0x30});
  auto regs = f.cpu.GetRegs();
  regs.P.E = false;
  regs.P.M = false;
  regs.P.X = false;
  regs.A = 0xBBAA;
  regs.X = 0xFFFD;
  regs.Y = 0x01FD;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 30);

  REQUIRE(r.completed_cycles == 30);
  const auto out = f.cpu.GetRegs();
  REQUIRE(out.P.M == true);
  REQUIRE(out.P.X == true);
  REQUIRE(out.A == 0xBBAA);  // B accumulator preserved
  REQUIRE(out.X == 0x00FD);
  REQUIRE(out.Y == 0x00FD);
}

TEST_CASE("PLP that sets X-flag in native mode zeroes X.H and Y.H", "[cpu][opcode]") {
  // PHP pushes P with X=0; we manually push a status byte with X=1, then PLP.
  // PLA path uses kLoadReg for Reg::kP, which must apply the same forcing as
  // SEP/REP/XCE.
  TestFixture f;
  // PLP only — we'll seed the stack ourselves.
  f.LoadAt(0, {0x28});
  auto regs = f.cpu.GetRegs();
  regs.P.E = false;
  regs.P.M = false;
  regs.P.X = false;
  regs.X = 0xDEAD;
  regs.Y = 0xBEEF;
  regs.SP = 0x01FE;
  f.cpu.SetRegs(regs);
  // Stack top byte ($01FF) becomes the pulled P. X bit is 0x10.
  f.snes.GetWram().WriteRegister(0x01FF, 0x10, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  const auto out = f.cpu.GetRegs();
  REQUIRE(out.P.X == true);
  REQUIRE(out.X == 0x00AD);
  REQUIRE(out.Y == 0x00EF);
}

TEST_CASE("TAX in emulation mode copies A low byte to X low and sets N/Z", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0xAA});
  auto regs = f.cpu.GetRegs();
  regs.A = 0x12C0;
  regs.X = 0x00FF;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 22);

  REQUIRE(r.completed_cycles == 22);
  const auto out = f.cpu.GetRegs();
  REQUIRE(out.X == 0x00C0);
  REQUIRE(out.P.N == true);
  REQUIRE(out.P.Z == false);
}

TEST_CASE("TXA 16-bit copies full 16 bits when M is clear", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x8A});
  auto regs = f.cpu.GetRegs();
  regs.P.E = false;
  regs.P.M = false;
  regs.X = 0xBEEF;
  regs.A = 0x1234;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 22);

  REQUIRE(r.completed_cycles == 22);
  REQUIRE(f.cpu.GetRegs().A == 0xBEEF);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("TXS in emulation mode forces SH back to $01", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x9A});
  auto regs = f.cpu.GetRegs();
  regs.X = 0xBEEF;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 22);

  REQUIRE(r.completed_cycles == 22);
  REQUIRE(f.cpu.GetRegs().SP == 0x01EF);
}

TEST_CASE("TXS in native 16-bit mode transfers the full 16 bits", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x9A});
  auto regs = f.cpu.GetRegs();
  regs.P.E = false;
  regs.P.X = false;
  regs.X = 0x1234;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 22);

  REQUIRE(r.completed_cycles == 22);
  REQUIRE(f.cpu.GetRegs().SP == 0x1234);
}

TEST_CASE("TCD transfers the full 16-bit accumulator to DP regardless of M", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x5B});
  auto regs = f.cpu.GetRegs();
  // Even with M=1 (8-bit A), TCD is a 16-bit transfer.
  regs.A = 0x8000;
  regs.DP = 0x0000;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 22);

  REQUIRE(r.completed_cycles == 22);
  REQUIRE(f.cpu.GetRegs().DP == 0x8000);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("TDC sets Z when the result is zero", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x7B});
  auto regs = f.cpu.GetRegs();
  regs.A = 0xFFFF;
  regs.DP = 0x0000;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 22);

  REQUIRE(r.completed_cycles == 22);
  REQUIRE(f.cpu.GetRegs().A == 0x0000);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}

TEST_CASE("TCS in emulation mode forces SH back to $01", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x1B});
  auto regs = f.cpu.GetRegs();
  regs.A = 0xBEEF;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 22);

  REQUIRE(r.completed_cycles == 22);
  REQUIRE(f.cpu.GetRegs().SP == 0x01EF);
}

TEST_CASE("TSC reads the full 16-bit SP into A", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x3B});
  auto regs = f.cpu.GetRegs();
  regs.SP = 0x01F0;
  regs.A = 0x0000;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 22);

  REQUIRE(r.completed_cycles == 22);
  REQUIRE(f.cpu.GetRegs().A == 0x01F0);
  REQUIRE(f.cpu.GetRegs().P.N == false);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("BEQ taken when Z=1 jumps forward", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0xF0, 0x02, 0xEA, 0xEA, 0xA9, 0x42});
  auto regs = f.cpu.GetRegs();
  regs.P.Z = true;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x42);
}

TEST_CASE("BCS not taken when C=0 falls through", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0xB0, 0x02, 0xA9, 0x11, 0xA9, 0x22});
  auto regs = f.cpu.GetRegs();
  regs.P.C = false;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x11);
}

TEST_CASE("BMI taken when N=1", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x30, 0x02, 0xEA, 0xEA, 0xA9, 0x99});
  auto regs = f.cpu.GetRegs();
  regs.P.N = true;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x99);
}

TEST_CASE("BVC taken when V=0", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x50, 0x02, 0xEA, 0xEA, 0xA9, 0x55});

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x55);
}

TEST_CASE("BRL applies signed 16-bit displacement in 4 cycles", "[cpu][opcode]") {
  TestFixture f;
  // BRL +0x0100 from $8000: PC_after_BRL_instr = $8003, target = $8103.
  f.LoadAt(0, {0x82, 0x00, 0x01});
  f.LoadAt(0x103, {0xA9, 0x77});

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.cpu.GetRegs().PC == 0x8105);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x77);
}

TEST_CASE("JMP absolute sets PC in 3 cycles", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x4C, 0x10, 0x80});
  f.LoadAt(0x10, {0xA9, 0x33});

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 40);

  REQUIRE(r.completed_cycles == 40);
  REQUIRE(f.cpu.GetRegs().PC == 0x8012);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x33);
}

TEST_CASE("ADC immediate 8-bit adds with carry", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x69, 0x01});
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0041;
  regs.P.C = true;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 24);

  REQUIRE(r.completed_cycles == 24);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x43);
  REQUIRE(f.cpu.GetRegs().P.C == false);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("ADC immediate 8-bit sets carry and overflow on wrap", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x69, 0x01});
  auto regs = f.cpu.GetRegs();
  regs.A = 0x007F;
  regs.P.C = false;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 24);

  REQUIRE(r.completed_cycles == 24);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x80);
  REQUIRE(f.cpu.GetRegs().P.V == true);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("ADC immediate 16-bit uses 3-cycle path", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x69, 0x34, 0x12});
  auto regs = f.cpu.GetRegs();
  regs.P.E = false;
  regs.P.M = false;
  regs.A = 0x1000;
  regs.P.C = false;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(f.cpu.GetRegs().A == 0x2234);
}

TEST_CASE("SBC immediate 8-bit subtracts with borrow", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0xE9, 0x01});
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0005;
  regs.P.C = true;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 24);

  REQUIRE(r.completed_cycles == 24);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x04);
  REQUIRE(f.cpu.GetRegs().P.C == true);
}

TEST_CASE("AND immediate masks A", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x29, 0x0F});
  auto regs = f.cpu.GetRegs();
  regs.A = 0x00A5;
  f.cpu.SetRegs(regs);

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x05);
}

TEST_CASE("ORA immediate ORs A", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x09, 0xF0});
  auto regs = f.cpu.GetRegs();
  regs.A = 0x000F;
  f.cpu.SetRegs(regs);

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xFF);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("EOR immediate XORs A", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x49, 0xFF});
  auto regs = f.cpu.GetRegs();
  regs.A = 0x005A;
  f.cpu.SetRegs(regs);

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xA5);
}

TEST_CASE("CMP immediate sets Z when equal and C when >=", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0xC9, 0x42, 0xC9, 0x43});
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0042;
  f.cpu.SetRegs(regs);

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 16);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 16);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
  REQUIRE(f.cpu.GetRegs().P.C == false);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("CPX immediate compares X", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0xE0, 0x10});
  auto regs = f.cpu.GetRegs();
  regs.X = 0x20;
  f.cpu.SetRegs(regs);

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(f.cpu.GetRegs().P.C == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("ADC direct page 8-bit adds value at DP+offset", "[cpu][opcode]") {
  ResetFixture f;
  f.LoadInstruction({0x65, 0x20});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0020, 0x05, 0);
  f.ModifyRegs([](auto& r) {
    r.A = 0x0040;
    r.P.C = false;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 24);  // 4-m+w with m=1,w=0 = 3

  REQUIRE(r.completed_cycles == 24);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x45);
  REQUIRE(f.cpu.GetRegs().PC == 0x8002);
}

TEST_CASE("ADC direct page 16-bit adds value at DP+offset", "[cpu][opcode]") {
  ResetFixture f;
  f.LoadInstruction({0x65, 0x40});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0040, 0x34, 0);
  f.wram.WriteRegister(0x0041, 0x12, 0);
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.P.M = false;
    r.A = 0x1000;
    r.P.C = false;
  });

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 32);  // 4-m+w with m=0,w=0 = 4

  REQUIRE(r.completed_cycles == 32);
  REQUIRE(f.cpu.GetRegs().A == 0x2234);
}

TEST_CASE("ADC direct page pays DL-nonzero penalty", "[cpu][opcode]") {
  ResetFixture f;
  f.LoadInstruction({0x65, 0x10});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.DP = 0x0123;
    r.A = 0x0001;
    r.P.C = false;
  });
  f.wram.WriteRegister(0x0133, 0x02, 0);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);  // 4-m+w with m=1,w=1 = 4

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x03);
}

TEST_CASE("SBC direct page subtracts with borrow", "[cpu][opcode]") {
  ResetFixture f;
  f.LoadInstruction({0xE5, 0x10});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0010, 0x01, 0);
  f.ModifyRegs([](auto& r) {
    r.A = 0x0005;
    r.P.C = true;
  });

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x04);
  REQUIRE(f.cpu.GetRegs().P.C == true);
}

TEST_CASE("AND/ORA/EOR direct page combine A with memory", "[cpu][opcode]") {
  ResetFixture f;
  // AND $10 ; ORA $11 ; EOR $12
  f.LoadInstruction({0x25, 0x10, 0x05, 0x11, 0x45, 0x12});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0010, 0x0F, 0);
  f.wram.WriteRegister(0x0011, 0xF0, 0);
  f.wram.WriteRegister(0x0012, 0xFF, 0);
  f.ModifyRegs([](auto& r) { r.A = 0x00A5; });

  // 3+3+3 = 9 cycles (all m=1, w=0).
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 72);

  REQUIRE(r.completed_cycles == 72);
  // A5 & 0F = 05 ; 05 | F0 = F5 ; F5 ^ FF = 0A
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x0A);
}

TEST_CASE("CMP direct page sets Z when equal", "[cpu][opcode]") {
  ResetFixture f;
  f.LoadInstruction({0xC5, 0x20});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0020, 0x42, 0);
  f.ModifyRegs([](auto& r) { r.A = 0x0042; });

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x42);  // A preserved
}

TEST_CASE("ASL A shifts accumulator left, bit 7 into C", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x0A});
  auto regs = f.cpu.GetRegs();
  regs.A = 0x00C3;
  regs.P.C = false;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 22);

  REQUIRE(r.completed_cycles == 22);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x86);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("ASL A 16-bit shifts full accumulator", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x0A});
  auto regs = f.cpu.GetRegs();
  regs.P.E = false;
  regs.P.M = false;
  regs.A = 0x4001;
  regs.P.C = false;
  f.cpu.SetRegs(regs);

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(f.cpu.GetRegs().A == 0x8002);
  REQUIRE(f.cpu.GetRegs().P.C == false);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("LSR A shifts right, bit 0 into C", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x4A});
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0003;
  regs.P.C = false;
  f.cpu.SetRegs(regs);

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x01);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}

TEST_CASE("ROL A rotates through carry", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x2A});
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0081;
  regs.P.C = true;
  f.cpu.SetRegs(regs);

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  // 0x81 << 1 | C=1 -> 0x03, C out = 1
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x03);
  REQUIRE(f.cpu.GetRegs().P.C == true);
}

TEST_CASE("ROR A rotates right through carry", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x6A});
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0002;
  regs.P.C = true;
  f.cpu.SetRegs(regs);

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  // 0x02 >> 1 with C=1 in bit 7 -> 0x81, C out = 0
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x81);
  REQUIRE(f.cpu.GetRegs().P.C == false);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("BIT immediate only affects Z", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x89, 0xF0});
  auto regs = f.cpu.GetRegs();
  regs.A = 0x0F;
  regs.P.N = false;
  regs.P.V = false;
  f.cpu.SetRegs(regs);

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(f.cpu.GetRegs().P.Z == true);
  // N and V should not be touched in immediate mode per the 65C816 manual.
  REQUIRE(f.cpu.GetRegs().P.N == false);
  REQUIRE(f.cpu.GetRegs().P.V == false);
}

TEST_CASE("PHX pushes X low byte in emulation mode", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0xDA});
  auto regs = f.cpu.GetRegs();
  regs.X = 0x55;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 30);

  REQUIRE(r.completed_cycles == 30);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FE);
}

TEST_CASE("PHP + PLP round-trip preserves P (respecting emulation)", "[cpu][opcode]") {
  TestFixture f;
  // PHP ; clear N+V+Z+C ; PLP  — should restore N=1, V=1.
  f.LoadAt(0, {0x08, 0x18, 0xB8, 0x28});
  auto regs = f.cpu.GetRegs();
  regs.P.N = true;
  regs.P.V = true;
  regs.P.C = true;
  f.cpu.SetRegs(regs);

  // PHP (3) + CLC (2) + CLV (2) + PLP (4) = 11
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 86);

  REQUIRE(r.completed_cycles == 86);
  const auto out = f.cpu.GetRegs().P;
  REQUIRE(out.N == true);
  REQUIRE(out.V == true);
  REQUIRE(out.C == true);
}

TEST_CASE("PHD + PLD round-trip restores DP with N/Z flags", "[cpu][opcode]") {
  TestFixture f;
  // PHD ; set DP=0 ; PLD
  f.LoadAt(0, {0x0B, 0x2B});
  auto regs = f.cpu.GetRegs();
  regs.DP = 0xBEEF;
  f.cpu.SetRegs(regs);

  TickResult r1 = f.cpu.TickToTarget(f.snes.GetMasterTime() + 30);  // PHD
  REQUIRE(r1.completed_cycles == 30);

  regs = f.cpu.GetRegs();
  regs.DP = 0;
  f.cpu.SetRegs(regs);

  TickResult r2 = f.cpu.TickToTarget(f.snes.GetMasterTime() + 44);  // PLD
  REQUIRE(r2.completed_cycles == 44);
  REQUIRE(f.cpu.GetRegs().DP == 0xBEEF);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("PHK pushes PBR to the stack", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x4B});

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 30);

  REQUIRE(r.completed_cycles == 30);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FE);
}

TEST_CASE("PEA pushes a 16-bit immediate (high byte first)", "[cpu][opcode]") {
  TestFixture f;
  // PEA $1234 — pushes $12 then $34.
  f.LoadAt(0, {0xF4, 0x34, 0x12});

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FD);
}

TEST_CASE("PLA 16-bit pulls both bytes when M is clear", "[cpu][opcode]") {
  TestFixture f;
  // PHA $ABCD ; reset A ; PLA
  f.LoadAt(0, {0x48, 0x68});
  auto regs = f.cpu.GetRegs();
  regs.P.E = false;
  regs.P.M = false;
  regs.A = 0xABCD;
  f.cpu.SetRegs(regs);

  // PHA 16-bit = 4 cycles, PLA 16-bit = 5 cycles = 9 total.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 74);

  REQUIRE(r.completed_cycles == 74);
  REQUIRE(f.cpu.GetRegs().A == 0xABCD);
}

TEST_CASE("JSR + RTS round trip", "[cpu][opcode]") {
  TestFixture f;
  // $8000: JSR $8010 ; $8003: LDA #$42 ; $8010: LDA #$77 ; RTS
  f.LoadAt(0x00, {0x20, 0x10, 0x80, 0xA9, 0x42});
  f.LoadAt(0x10, {0xA9, 0x77, 0x60});

  // JSR (6) + LDA (2) + RTS (6) + LDA (2) = 16 cycles
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 128);

  REQUIRE(r.completed_cycles == 128);
  // After RTS returns, LDA #$42 has run.
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x42);
  // SP restored back to $01FF.
  REQUIRE(f.cpu.GetRegs().SP == 0x01FF);
}

TEST_CASE("JSL + RTL round trip crosses banks and restores PBR", "[cpu][opcode]") {
  TestFixture f;
  // Map page $01:$80 so the JSL target exists. Reuse the same ROM but pretend
  // bank 1 for the test by using absolute long within bank 0.
  // $8000: JSL $00:$8010 ; $8004: LDA #$11 ; $8010: LDA #$22 ; RTL
  f.LoadAt(0x00, {0x22, 0x10, 0x80, 0x00, 0xA9, 0x11});
  f.LoadAt(0x10, {0xA9, 0x22, 0x6B});

  // JSL (8) + LDA (2) + RTL (6) + LDA (2) = 18 cycles
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 146);

  REQUIRE(r.completed_cycles == 146);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x11);
  REQUIRE(f.cpu.GetRegs().PBR == 0x00);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FF);
}

TEST_CASE("JMP absolute long sets PC and PBR", "[cpu][opcode]") {
  TestFixture f;
  // JMP $00:8020 — within-bank long jump.
  f.LoadAt(0, {0x5C, 0x20, 0x80, 0x00});
  f.LoadAt(0x20, {0xA9, 0xAB});

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);

  REQUIRE(r.completed_cycles == 48);
  REQUIRE(f.cpu.GetRegs().PC == 0x8022);
  REQUIRE(f.cpu.GetRegs().PBR == 0x00);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0xAB);
}

TEST_CASE("JMP (abs) indirect loads PC through a bank-0 pointer", "[cpu][opcode]") {
  TestFixture f;
  // $8000: JMP ($8100) ; pointer at $00:8100 = $8020 ; $8020: LDA #$55
  f.LoadAt(0, {0x6C, 0x00, 0x81});
  f.LoadAt(0x100, {0x20, 0x80});
  f.LoadAt(0x20, {0xA9, 0x55});

  // JMP (abs) = 5 cycles, LDA #$55 = 2 cycles; 7 × 8 = 56 master cycles.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 56);

  REQUIRE(r.completed_cycles == 56);
  REQUIRE(f.cpu.GetRegs().PC == 0x8022);
  REQUIRE(f.cpu.GetRegs().PBR == 0x00);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x55);
}

TEST_CASE("JMP [abs] indirect long loads PC+PBR through a 24-bit pointer", "[cpu][opcode]") {
  TestFixture f;
  // $8000: JMP [$8100] ; pointer at $00:8100 = $00:8020 ; target: LDA #$66
  f.LoadAt(0, {0xDC, 0x00, 0x81});
  f.LoadAt(0x100, {0x20, 0x80, 0x00});
  f.LoadAt(0x20, {0xA9, 0x66});

  // JMP [abs] = 6 cycles, LDA #$66 = 2; 8 × 8 = 64 master cycles.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 64);

  REQUIRE(r.completed_cycles == 64);
  REQUIRE(f.cpu.GetRegs().PC == 0x8022);
  REQUIRE(f.cpu.GetRegs().PBR == 0x00);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x66);
}

TEST_CASE("JMP (abs,X) indirect uses program-bank pointer with X offset", "[cpu][opcode]") {
  TestFixture f;
  // $8000: JMP ($80FE,X) with X=$02 → pointer at $00:8100 = $8020
  f.LoadAt(0, {0x7C, 0xFE, 0x80});
  f.LoadAt(0x100, {0x20, 0x80});
  f.LoadAt(0x20, {0xA9, 0x77});
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0002;
  f.cpu.SetRegs(regs);

  // JMP (abs,X) = 6 cycles (5 bus + 1 internal) + LDA = 2 bus
  //             = 7 × 8 + 1 × 6 = 62 master cycles.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 62);

  REQUIRE(r.completed_cycles == 62);
  REQUIRE(f.cpu.GetRegs().PC == 0x8022);
  REQUIRE(f.cpu.GetRegs().PBR == 0x00);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x77);
}

TEST_CASE("ASL dp 8-bit shifts memory and sets carry", "[cpu][opcode][rmw]") {
  ResetFixture f;
  // ASL $10 with DP=0 → effective addr $0010.
  f.LoadInstruction({0x06, 0x10});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0010, 0x81, 0);

  // ASL dp 8-bit = 5 cycles = 4 bus × 8 + 1 internal × 6 = 38 master cycles.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 38);

  REQUIRE(r.completed_cycles == 38);
  REQUIRE(f.wram.ReadRegister(0x0010, 0).value == 0x02);  // 0x81 << 1 = 0x102 → 0x02
  REQUIRE(f.cpu.GetRegs().P.C == true);                   // bit 7 went to C
}

TEST_CASE("INC abs 8-bit increments memory", "[cpu][opcode][rmw]") {
  ResetFixture f;
  // INC $1234
  f.LoadInstruction({0xEE, 0x34, 0x12});

  f.cpu.Reset();
  f.wram.WriteRegister(0x1234, 0x7F, 0);

  // INC abs 8-bit = 6 cycles = 5 bus × 8 + 1 internal × 6 = 46 master cycles.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.wram.ReadRegister(0x1234, 0).value == 0x80);
  REQUIRE(f.cpu.GetRegs().P.N == true);
}

TEST_CASE("DEC abs 16-bit decrements 16-bit memory", "[cpu][opcode][rmw]") {
  ResetFixture f;
  // DEC $1234 with M=0
  f.LoadInstruction({0xCE, 0x34, 0x12});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.P.M = false;
  });
  f.wram.WriteRegister(0x1234, 0x00, 0);
  f.wram.WriteRegister(0x1235, 0x01, 0);  // 16-bit value 0x0100

  // DEC abs 16-bit = 8 cycles = 7 bus × 8 + 1 internal × 6 = 62 master cycles.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 62);

  REQUIRE(r.completed_cycles == 62);
  REQUIRE(f.wram.ReadRegister(0x1234, 0).value == 0xFF);
  REQUIRE(f.wram.ReadRegister(0x1235, 0).value == 0x00);  // 0x0100 - 1 = 0x00FF
}

TEST_CASE("INC abs 8-bit at $FFFF stays in DBR-bank across read/write", "[cpu][opcode][rmw]") {
  // Regression: INC $FFFF with DBR=$7E reads and writes a single byte at
  // $7E:FFFF. The internal advance/rollback bookkeeping must stay 24-bit so
  // the write-back doesn't escape to the previous bank.
  ResetFixture f;
  f.LoadInstruction({0xEE, 0xFF, 0xFF});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0FFFF, 0x7F, 0);  // $7E:FFFF — target
  f.wram.WriteRegister(0x0FFFE, 0xAA, 0);  // $7E:FFFE — must not be clobbered
  f.wram.WriteRegister(0x10000, 0xBB, 0);  // $7F:0000 — must not be clobbered
  f.ModifyRegs([](auto& r) {
    r.DBR = 0x7E;
    r.P.M = true;  // 8-bit accumulator/memory
  });

  // INC abs 8-bit = 6 cycles × 8 master = 46 (5 mem accesses at 8 + 1 internal at 6).
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.wram.Peek(0x0FFFF) == 0x80);
  REQUIRE(f.wram.Peek(0x0FFFE) == 0xAA);
  REQUIRE(f.wram.Peek(0x10000) == 0xBB);
}

TEST_CASE("INC abs 16-bit at $FFFF carries operand fetch into next bank", "[cpu][opcode][rmw]") {
  // Regression: 16-bit INC $FFFF with DBR=$7E reads/writes a 16-bit operand
  // that straddles $7E:FFFF (low) and $7F:0000 (high).
  ResetFixture f;
  f.LoadInstruction({0xEE, 0xFF, 0xFF});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.DBR = 0x7E;
    r.P.E = false;
    r.P.M = false;  // 16-bit memory
  });
  f.wram.WriteRegister(0x0FFFF, 0xFF, 0);  // $7E:FFFF (low byte)
  f.wram.WriteRegister(0x10000, 0x00, 0);  // $7F:0000 (high byte) -> 16-bit value $00FF
  f.wram.WriteRegister(0x00000, 0x5A, 0);  // $7E:0000 — must NOT be touched

  // INC abs 16-bit = 8 cycles = 7 mem × 8 + 1 internal × 6 = 62 master cycles.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 62);

  REQUIRE(r.completed_cycles == 62);
  // $00FF + 1 = $0100 -> low byte $00 at $7E:FFFF, high byte $01 at $7F:0000.
  REQUIRE(f.wram.Peek(0x0FFFF) == 0x00);
  REQUIRE(f.wram.Peek(0x10000) == 0x01);
  REQUIRE(f.wram.Peek(0x00000) == 0x5A);  // bank-wrap target untouched
}

TEST_CASE("ROL dp,X 8-bit rotates memory through carry", "[cpu][opcode][rmw]") {
  ResetFixture f;
  // ROL $10,X with X=$02, DP=0 → effective addr $0012. C=1 rotates into bit 0.
  f.LoadInstruction({0x36, 0x10});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.X = 0x0002;
    r.P.C = true;
  });
  f.wram.WriteRegister(0x0012, 0x40, 0);

  // ROL dp,X 8-bit = 6 cycles = 5 bus × 8 + 1 internal × 6 = 46 master cycles.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);

  REQUIRE(r.completed_cycles == 46);
  REQUIRE(f.wram.ReadRegister(0x0012, 0).value == 0x81);  // (0x40 << 1) | 1 = 0x81
  REQUIRE(f.cpu.GetRegs().P.C == false);                  // bit 7 of original (0x40) was 0
}

TEST_CASE("ASL abs,X 16-bit shifts 16-bit memory", "[cpu][opcode][rmw]") {
  ResetFixture f;
  // ASL $1230,X with X=4, M=0 → effective addr $1234.
  f.LoadInstruction({0x1E, 0x30, 0x12});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.P.M = false;
    r.X = 0x0004;
  });
  f.wram.WriteRegister(0x1234, 0x00, 0);
  f.wram.WriteRegister(0x1235, 0x80, 0);  // 16-bit value 0x8000

  // ASL abs,X 16-bit = 9 cycles = 8 bus × 8 + 1 internal × 6 = 70 master cycles.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 70);

  REQUIRE(r.completed_cycles == 70);
  REQUIRE(f.wram.ReadRegister(0x1234, 0).value == 0x00);
  REQUIRE(f.wram.ReadRegister(0x1235, 0).value == 0x00);  // 0x8000 << 1 = 0x10000 → 0x0000
  REQUIRE(f.cpu.GetRegs().P.C == true);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

TEST_CASE("JSR (abs,X) pushes return address and jumps through pointer", "[cpu][opcode]") {
  TestFixture f;
  // $8000: JSR ($80FE,X) with X=$02 → pointer at $00:8100 = $8020
  // Target at $8020: LDA #$88 ; RTS
  f.LoadAt(0, {0xFC, 0xFE, 0x80, 0xA9, 0x11});
  f.LoadAt(0x100, {0x20, 0x80});
  f.LoadAt(0x20, {0xA9, 0x88, 0x60});
  auto regs = f.cpu.GetRegs();
  regs.X = 0x0002;
  f.cpu.SetRegs(regs);

  // Master-cycle breakdown:
  //   JSR (abs,X): 8 cycles = 7 bus (8 each) + 1 internal (6) = 62
  //   LDA #$88:    2 bus = 16
  //   RTS:         6 cycles = 3 bus (8 each) + 3 internal (6 each) = 42
  //   LDA #$11:    16
  //   Total:       136 master cycles
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 136);

  REQUIRE(r.completed_cycles == 136);
  // After JSR ($80FE,X) → $8020, LDA #$88, RTS to $8003, LDA #$11 → A=$11.
  REQUIRE(f.cpu.GetRegs().PC == 0x8005);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x11);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FF);
}

TEST_CASE("TXY transfers X to Y using the index width", "[cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x9B});
  auto regs = f.cpu.GetRegs();
  regs.P.E = false;
  regs.P.X = false;
  regs.X = 0x4321;
  regs.Y = 0xAAAA;
  f.cpu.SetRegs(regs);

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 22);

  REQUIRE(r.completed_cycles == 22);
  REQUIRE(f.cpu.GetRegs().Y == 0x4321);
}

TEST_CASE("REP in emulation mode cannot clear M or X", "[cpu][opcode]") {
  TestFixture f;
  // E=1; REP #$30 should NOT clear M/X because emulation forces them to 1.
  f.LoadAt(0, {0xC2, 0x30});

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 30);

  REQUIRE(r.completed_cycles == 30);
  const auto out = f.cpu.GetRegs().P;
  REQUIRE(out.M == true);
  REQUIRE(out.X == true);
}

TEST_CASE("CPX dp compares X to memory and sets flags", "[unit][cpu][opcode]") {
  ResetFixture f;
  f.LoadInstruction({0xE4, 0x20});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0020, 0x42, 0);
  f.ModifyRegs([](auto& r) { r.X = 0x0042; });

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.C == true);
  REQUIRE(f.cpu.GetRegs().P.N == false);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().X) == 0x42);
}

TEST_CASE("CPY dp compares Y to memory and sets carry/negative", "[unit][cpu][opcode]") {
  ResetFixture f;
  f.LoadInstruction({0xC4, 0x20});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0020, 0x80, 0);
  f.ModifyRegs([](auto& r) { r.Y = 0x0001; });

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  // 0x01 - 0x80 = 0x81 (N=1), and Y < mem so C=0.
  REQUIRE(f.cpu.GetRegs().P.Z == false);
  REQUIRE(f.cpu.GetRegs().P.C == false);
  REQUIRE(f.cpu.GetRegs().P.N == true);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().Y) == 0x01);
}

TEST_CASE("TSB dp sets memory bits from A and updates Z from (A AND mem)", "[unit][cpu][opcode][rmw]") {
  ResetFixture f;
  f.LoadInstruction({0x04, 0x10});

  f.cpu.Reset();
  f.wram.WriteRegister(0x0010, 0x0C, 0);
  f.ModifyRegs([](auto& r) {
    r.A = 0x0003;
    r.P.N = true;  // must be preserved
    r.P.C = true;  // must be preserved
  });

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  // mem was 0x0C, A=0x03; (A & mem) = 0 → Z=1; result mem = 0x0C | 0x03 = 0x0F.
  REQUIRE(f.wram.ReadRegister(0x0010, 0).value == 0x0F);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.N == true);                      // preserved
  REQUIRE(f.cpu.GetRegs().P.C == true);                      // preserved
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x03);  // A unchanged
}

TEST_CASE("TRB abs clears memory bits from A and leaves Z=0 on overlap", "[unit][cpu][opcode][rmw]") {
  ResetFixture f;
  f.LoadInstruction({0x1C, 0x34, 0x12});

  f.cpu.Reset();
  f.wram.WriteRegister(0x1234, 0xFF, 0);
  f.ModifyRegs([](auto& r) { r.A = 0x0055; });

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  // (A & mem) = 0x55 → Z=0; result mem = 0xFF & ~0x55 = 0xAA.
  REQUIRE(f.wram.ReadRegister(0x1234, 0).value == 0xAA);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
  REQUIRE(static_cast<uint8_t>(f.cpu.GetRegs().A) == 0x55);
}

TEST_CASE("TSB abs 16-bit ORs a 16-bit value and uses 16-bit Z", "[unit][cpu][opcode][rmw]") {
  ResetFixture f;
  f.LoadInstruction({0x0C, 0x34, 0x12});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.P.M = false;
    r.A = 0x0000;  // AND with zero → Z=1, mem unchanged
  });
  f.wram.WriteRegister(0x1234, 0xAA, 0);
  f.wram.WriteRegister(0x1235, 0x55, 0);

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  // A=0 so result = mem | 0 = unchanged; Z=1 from (A & mem) == 0.
  REQUIRE(f.wram.ReadRegister(0x1234, 0).value == 0xAA);
  REQUIRE(f.wram.ReadRegister(0x1235, 0).value == 0x55);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
}

TEST_CASE("WDM is a 2-byte, 2-cycle no-op that advances PC by 2", "[unit][cpu][opcode]") {
  TestFixture f;
  f.LoadAt(0, {0x42, 0xAB});

  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 16);

  // 2 cycles × 8 master (FetchPc on slow rom) = 16 master.
  REQUIRE(r.completed_cycles == 16);
  REQUIRE(f.cpu.GetRegs().PC == 0x8002);
}

TEST_CASE("XBA swaps A high and low bytes and sets N/Z from new low byte", "[unit][cpu][opcode]") {
  ResetFixture f;
  f.LoadInstruction({0xEB});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.P.M = false;
    r.A = 0x6789;  // B=0x67, A=0x89
  });

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  // After swap: A = 0x8967. New low byte = 0x67 → N=0, Z=0.
  REQUIRE(f.cpu.GetRegs().A == 0x8967);
  REQUIRE(f.cpu.GetRegs().P.N == false);
  REQUIRE(f.cpu.GetRegs().P.Z == false);
}

TEST_CASE("XBA with high byte zero sets Z regardless of M flag", "[unit][cpu][opcode]") {
  ResetFixture f;
  f.LoadInstruction({0xEB});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) { r.A = 0x0080; });  // B=0x00, A=0x80, M=1

  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 100);

  // After swap A = 0x8000. New low byte = 0x00 → Z=1, N=0.
  REQUIRE(f.cpu.GetRegs().A == 0x8000);
  REQUIRE(f.cpu.GetRegs().P.Z == true);
  REQUIRE(f.cpu.GetRegs().P.N == false);
}

TEST_CASE("PEI pushes 16-bit value read from DP+offset, high first", "[unit][cpu][opcode]") {
  ResetFixture f;
  f.LoadInstruction({0xD4, 0x10});

  f.cpu.Reset();
  // Value at $0010 (little-endian): $BEEF → low=$EF at $0010, high=$BE at $0011.
  f.wram.WriteRegister(0x0010, 0xEF, 0);
  f.wram.WriteRegister(0x0011, 0xBE, 0);

  const uint16_t sp_before = f.cpu.GetRegs().SP;
  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 200);

  // Push order: high to $sp, low to $sp-1; final SP = sp_before - 2.
  REQUIRE(f.wram.ReadRegister(sp_before, 0).value == 0xBE);
  REQUIRE(f.wram.ReadRegister(static_cast<uint16_t>(sp_before - 1U), 0).value == 0xEF);
  REQUIRE(f.cpu.GetRegs().SP == static_cast<uint16_t>(sp_before - 2U));
}

TEST_CASE("PER pushes (PC + signed displacement) after the instruction", "[unit][cpu][opcode]") {
  ResetFixture f;
  // Opcode at $8000; displacement $0020 → effective addr = ($8003 + $0020) = $8023.
  f.LoadInstruction({0x62, 0x20, 0x00});

  f.cpu.Reset();
  const uint16_t sp_before = f.cpu.GetRegs().SP;

  // PER = 6 cycles: 5 bus × 8 + 1 internal × 6 = 46 master cycles.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 46);
  REQUIRE(r.completed_cycles == 46);

  // High = $80, low = $23.
  REQUIRE(f.wram.ReadRegister(sp_before, 0).value == 0x80);
  REQUIRE(f.wram.ReadRegister(static_cast<uint16_t>(sp_before - 1U), 0).value == 0x23);
  REQUIRE(f.cpu.GetRegs().SP == static_cast<uint16_t>(sp_before - 2U));
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
}

TEST_CASE("BRK in emulation mode jumps through $FFFE, pushes PC+2 and P", "[unit][cpu][opcode]") {
  ResetFixture f;
  f.LoadInstruction({0x00, 0x42});  // BRK + signature
  // Emulation BRK vector at $00:FFFE → handler $8100.
  f.SetRomByte(0x7FFE, 0x00);
  f.SetRomByte(0x7FFF, 0x81);
  f.SyncCartridge();

  f.cpu.Reset();
  const uint16_t sp_before = f.cpu.GetRegs().SP;

  // BRK emulation = 7 cycles × 8 master = 56.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 56);
  REQUIRE(r.completed_cycles == 56);

  REQUIRE(f.cpu.GetRegs().PC == 0x8100);
  REQUIRE(f.cpu.GetRegs().PBR == 0x00);
  REQUIRE(f.cpu.GetRegs().P.I == true);
  REQUIRE(f.cpu.GetRegs().P.D == false);
  // Pushed (top-down): PCH=$80 at $sp, PCL=$02 at $sp-1, P at $sp-2.
  REQUIRE(f.wram.ReadRegister(sp_before, 0).value == 0x80);
  REQUIRE(f.wram.ReadRegister(static_cast<uint16_t>(sp_before - 1U), 0).value == 0x02);
  REQUIRE(f.cpu.GetRegs().SP == static_cast<uint16_t>(sp_before - 3U));
}

TEST_CASE("BRK in native mode pushes PBR and uses the $FFE6 vector", "[unit][cpu][opcode]") {
  ResetFixture f;
  f.LoadInstruction({0x00, 0x42});
  // Native BRK vector at $00:FFE6.
  f.SetRomByte(0x7FE6, 0x00);
  f.SetRomByte(0x7FE7, 0x90);
  f.SyncCartridge();

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.P.E = false;  // enter native mode
    r.PBR = 0x80;
  });
  const uint16_t sp_before = f.cpu.GetRegs().SP;

  // BRK native = 8 cycles × 8 master = 64.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 64);
  REQUIRE(r.completed_cycles == 64);

  REQUIRE(f.cpu.GetRegs().PC == 0x9000);
  REQUIRE(f.cpu.GetRegs().PBR == 0x00);
  // Native push order: PBR, PCH, PCL, P.
  REQUIRE(f.wram.ReadRegister(sp_before, 0).value == 0x80);                              // PBR
  REQUIRE(f.wram.ReadRegister(static_cast<uint16_t>(sp_before - 1U), 0).value == 0x80);  // PCH
  REQUIRE(f.wram.ReadRegister(static_cast<uint16_t>(sp_before - 2U), 0).value == 0x02);  // PCL
  REQUIRE(f.cpu.GetRegs().SP == static_cast<uint16_t>(sp_before - 4U));
}

TEST_CASE("COP jumps through its own emulation vector at $FFF4", "[unit][cpu][opcode]") {
  ResetFixture f;
  f.LoadInstruction({0x02, 0x55});  // COP + signature
  f.SetRomByte(0x7FF4, 0x00);
  f.SetRomByte(0x7FF5, 0x82);
  f.SyncCartridge();

  f.cpu.Reset();
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 56);
  REQUIRE(r.completed_cycles == 56);

  REQUIRE(f.cpu.GetRegs().PC == 0x8200);
  REQUIRE(f.cpu.GetRegs().PBR == 0x00);
  REQUIRE(f.cpu.GetRegs().P.I == true);
  REQUIRE(f.cpu.GetRegs().P.D == false);
}

TEST_CASE("RTI in emulation pulls P then 16-bit PC", "[unit][cpu][opcode]") {
  ResetFixture f;
  f.LoadInstruction({0x40});

  f.cpu.Reset();
  // Pre-populate stack (emulation uses page 1). SP starts at $01FF.
  f.ModifyRegs([](auto& r) {
    r.SP = 0x01FC;
    r.P.I = true;
    r.P.D = true;
  });
  // After RTI with SP=$01FC: first increments SP and pulls P from $01FD,
  // then PCL from $01FE, then PCH from $01FF.
  f.wram.WriteRegister(0x01FD, 0x30, 0);  // P byte: M=1, Z=1 (just to see restore)
  f.wram.WriteRegister(0x01FE, 0x34, 0);  // PCL
  f.wram.WriteRegister(0x01FF, 0x12, 0);  // PCH

  // RTI emulation = 6 cycles × 8 master = 48.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 48);
  REQUIRE(r.completed_cycles == 48);

  REQUIRE(f.cpu.GetRegs().PC == 0x1234);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FF);
  // $30 in P means M=1, X=1 (bits 5,4) set; D,I,Z,C are 0. In emulation
  // M/X stay forced to 1 so the observable changes are D=0 (was 1), I=0, Z=0.
  REQUIRE(f.cpu.GetRegs().P.D == false);
  REQUIRE(f.cpu.GetRegs().P.I == false);
}

TEST_CASE("RTI in native also pulls PBR", "[unit][cpu][opcode]") {
  ResetFixture f;
  f.LoadInstruction({0x40});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.SP = 0x01FB;
    r.PBR = 0x80;
  });
  f.wram.WriteRegister(0x01FC, 0x00, 0);  // P
  f.wram.WriteRegister(0x01FD, 0x34, 0);  // PCL
  f.wram.WriteRegister(0x01FE, 0x12, 0);  // PCH
  f.wram.WriteRegister(0x01FF, 0x42, 0);  // PBR

  // RTI native = 7 cycles × 8 master = 56.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 56);
  REQUIRE(r.completed_cycles == 56);

  REQUIRE(f.cpu.GetRegs().PC == 0x1234);
  REQUIRE(f.cpu.GetRegs().PBR == 0x42);
  REQUIRE(f.cpu.GetRegs().SP == 0x01FF);
}

TEST_CASE("STP halts the CPU: subsequent ticks do not advance PC", "[unit][cpu][opcode]") {
  ResetFixture f;
  // STP at $8000, followed by an INX ($E8) we must NOT execute.
  f.LoadInstruction({0xDB, 0xE8, 0xE8, 0xE8});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) { r.X = 0x0001; });

  // STP = 3 cycles × 8 master = 24.
  TickResult r1 = f.cpu.TickToTarget(f.snes.GetMasterTime() + 24);
  REQUIRE(r1.completed_cycles == 24);
  // PC advanced past the 1-byte STP.
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetRegs().X == 0x0001);

  // Further ticks should consume time but not execute any further instructions.
  TickResult r2 = f.cpu.TickToTarget(f.snes.GetMasterTime() + 200);
  REQUIRE(r2.completed_cycles == 200);
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetRegs().X == 0x0001);
}

TEST_CASE("MVN copies a block forward one byte per 7-cycle pass", "[unit][cpu][opcode]") {
  ResetFixture f;
  // MVN #$00,#$00 — src bank = dest bank = $00. Operand layout on 65C816 is
  // dest bank byte first, then source bank (Clark §6.6).
  f.LoadInstruction({0x54, 0x00, 0x00});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.P.E = false;  // enable 16-bit A/X/Y
    r.P.M = false;
    r.P.X = false;
    r.A = 0x0002;  // 3 bytes: count - 1
    r.X = 0x0300;  // source low 16
    r.Y = 0x0400;  // destination low 16
    r.DBR = 0x55;  // will be overwritten by dest bank ($00)
  });
  f.wram.WriteRegister(0x0300, 0xAA, 0);
  f.wram.WriteRegister(0x0301, 0xBB, 0);
  f.wram.WriteRegister(0x0302, 0xCC, 0);
  // Clear the destination region.
  f.wram.WriteRegister(0x0400, 0x00, 0);
  f.wram.WriteRegister(0x0401, 0x00, 0);
  f.wram.WriteRegister(0x0402, 0x00, 0);

  // MVN per iteration = 5 bus (8 master each) + 2 internal (6 master each) =
  // 52 master. 3 iterations = 156 master.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 156);
  REQUIRE(r.completed_cycles == 156);

  REQUIRE(f.wram.ReadRegister(0x0400, 0).value == 0xAA);
  REQUIRE(f.wram.ReadRegister(0x0401, 0).value == 0xBB);
  REQUIRE(f.wram.ReadRegister(0x0402, 0).value == 0xCC);
  REQUIRE(f.cpu.GetRegs().A == 0xFFFF);
  REQUIRE(f.cpu.GetRegs().X == 0x0303);
  REQUIRE(f.cpu.GetRegs().Y == 0x0403);
  REQUIRE(f.cpu.GetRegs().DBR == 0x00);
  // PC advanced past the 3-byte instruction after the final iteration.
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
}

TEST_CASE("MVP copies a block backward one byte per 7-cycle pass", "[unit][cpu][opcode]") {
  ResetFixture f;
  // MVP #$00,#$00 — dest and src bank both $00.
  f.LoadInstruction({0x44, 0x00, 0x00});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) {
    r.P.E = false;
    r.P.M = false;
    r.P.X = false;
    r.A = 0x0002;  // 3 bytes to move
    r.X = 0x0302;  // source = end of block (MVP walks downward)
    r.Y = 0x0402;  // destination = end of block
    r.DBR = 0x55;
  });
  f.wram.WriteRegister(0x0300, 0xAA, 0);
  f.wram.WriteRegister(0x0301, 0xBB, 0);
  f.wram.WriteRegister(0x0302, 0xCC, 0);
  f.wram.WriteRegister(0x0400, 0x00, 0);
  f.wram.WriteRegister(0x0401, 0x00, 0);
  f.wram.WriteRegister(0x0402, 0x00, 0);

  // MVP per iteration = 5 bus × 8 + 2 internal × 6 = 52 master. 3 iterations = 156.
  TickResult r = f.cpu.TickToTarget(f.snes.GetMasterTime() + 156);
  REQUIRE(r.completed_cycles == 156);

  REQUIRE(f.wram.ReadRegister(0x0400, 0).value == 0xAA);
  REQUIRE(f.wram.ReadRegister(0x0401, 0).value == 0xBB);
  REQUIRE(f.wram.ReadRegister(0x0402, 0).value == 0xCC);
  REQUIRE(f.cpu.GetRegs().A == 0xFFFF);
  REQUIRE(f.cpu.GetRegs().X == 0x02FF);
  REQUIRE(f.cpu.GetRegs().Y == 0x03FF);
  REQUIRE(f.cpu.GetRegs().DBR == 0x00);
  REQUIRE(f.cpu.GetRegs().PC == 0x8003);
}

TEST_CASE("WAI halts the CPU until reset", "[unit][cpu][opcode]") {
  ResetFixture f;
  f.LoadInstruction({0xCB, 0xE8, 0xE8});

  f.cpu.Reset();
  f.ModifyRegs([](auto& r) { r.X = 0x0010; });

  TickResult r1 = f.cpu.TickToTarget(f.snes.GetMasterTime() + 24);
  REQUIRE(r1.completed_cycles == 24);

  TickResult r2 = f.cpu.TickToTarget(f.snes.GetMasterTime() + 128);
  REQUIRE(r2.completed_cycles == 128);
  REQUIRE(f.cpu.GetRegs().PC == 0x8001);
  REQUIRE(f.cpu.GetRegs().X == 0x0010);  // INX never executed
}

TEST_CASE("PER with negative displacement wraps within the bank", "[unit][cpu][opcode]") {
  ResetFixture f;
  // Opcode at $8000; disp = $FFF0 (-16) → effective = ($8003 - 16) = $7FF3.
  f.LoadInstruction({0x62, 0xF0, 0xFF});

  f.cpu.Reset();
  const uint16_t sp_before = f.cpu.GetRegs().SP;
  (void)f.cpu.TickToTarget(f.snes.GetMasterTime() + 200);

  REQUIRE(f.wram.ReadRegister(sp_before, 0).value == 0x7F);
  REQUIRE(f.wram.ReadRegister(static_cast<uint16_t>(sp_before - 1U), 0).value == 0xF3);
}
