#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>

#include "pupsnes/core/snes.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/apu/apu.h"
#include "pupsnes/hw/apu/spc700.h"
#include "pupsnes/memory/systembus.h"

namespace {
using pupsnes::Apu;
using pupsnes::SNES;
using pupsnes::Spc700;
using pupsnes::TimeMasterT;

void PreparePortCopy(Apu& apu) {
  // MOV A,$F4; MOV $F5,A; BRA $0200.
  constexpr std::array<uint8_t, 6> kProgram = {0xE4, 0xF4, 0xC4, 0xF5, 0x2F, 0xFA};
  for (std::size_t i = 0; i < kProgram.size(); ++i) apu.Write(static_cast<uint16_t>(0x0200 + i), kProgram[i]);
  Spc700::State state;
  state.pc = 0x0200;
  apu.GetCpu().Reset(state);
}
}  // namespace

TEST_CASE("APU executes IPL to produce its ready signature", "[unit][apu]") {
  SNES snes;
  auto& apu = snes.GetApu();
  REQUIRE(apu.ReadRegister(0x2140, 0).value == 0);
  REQUIRE(apu.ReadRegister(0x2141, 0).value == 0);
  apu.CatchUpTo(100000);
  REQUIRE_FALSE(apu.GetCpu().GetState().faulted);
  REQUIRE(apu.GetCpu().GetState().cycles > 0);
  REQUIRE(apu.GetPort(0) == 0xAA);
  REQUIRE(apu.GetPort(1) == 0xBB);
  REQUIRE(apu.PeekRam(0xF4) == 0xAA);
  REQUIRE(apu.PeekRam(0xF5) == 0xBB);
  // Polling cannot invent any new response or alter the SPC output latches.
  for (TimeMasterT time = 100000; time < 101000; ++time) REQUIRE(apu.ReadRegister(0x2140, time).value == 0xAA);
}

TEST_CASE("APU clock preserves fractional cycles and never runs ahead", "[unit][apu]") {
  SNES whole;
  SNES sliced;
  auto& apu = whole.GetApu();
  apu.CatchUpTo(20);
  REQUIRE(apu.GetCpu().GetState().cycles == 0);
  apu.CatchUpTo(21);
  REQUIRE(apu.GetCpu().GetState().cycles == 1);
  apu.CatchUpTo(41);
  REQUIRE(apu.GetCpu().GetState().cycles == 1);
  apu.CatchUpTo(42);
  REQUIRE(apu.GetCpu().GetState().cycles == 2);
  REQUIRE(apu.GetCpu().GetState().x == 0xEF);
  apu.CatchUpTo(Apu::kClockDenominator);
  for (TimeMasterT time = 1; time <= Apu::kClockDenominator; ++time) sliced.GetApu().CatchUpTo(time);
  REQUIRE(apu.GetCpu().GetState().cycles == Apu::kClockNumerator);
  REQUIRE(apu.GetClockPhase() == 0);
  REQUIRE(sliced.GetApu().GetClockPhase() == 0);
  REQUIRE(sliced.GetApu().GetCpu().GetState().cycles == apu.GetCpu().GetState().cycles);
  REQUIRE(sliced.GetApu().GetCpu().GetState().pc == apu.GetCpu().GetState().pc);
  REQUIRE(sliced.GetApu().GetRam() == apu.GetRam());
}

TEST_CASE("APU CPU and SPC ports have independent directional latches", "[unit][apu]") {
  SNES snes;
  auto& apu = snes.GetApu();
  apu.WriteRegister(0x2140, 0x17, 0);
  REQUIRE(apu.Read(0xF4) == 0x17);
  REQUIRE(apu.ReadRegister(0x2140, 0).value == 0);
  apu.Write(0xF4, 0x91);
  REQUIRE(apu.Read(0xF4) == 0x17);
  REQUIRE(apu.ReadRegister(0x2140, 0).value == 0x91);
  REQUIRE(apu.ReadRegister(0x2140, 0).driven_mask == 0xFF);
}

TEST_CASE("APU port mirrors reach the real device through the system bus", "[unit][apu]") {
  SNES snes;
  auto& bus = snes.GetSystemBus();
  auto& apu = snes.GetApu();
  using pupsnes::BusAccessType;
  auto plan = bus.Plan(0xBF217E, BusAccessType::kWrite, 0x37);
  REQUIRE(plan.access_cycles == 6);
  static_cast<void>(bus.Follow(plan, 0, snes.GetCpu().GetDeviceId()));
  REQUIRE(apu.Read(0xF6) == 0x37);
  apu.Write(0xF6, 0x62);
  plan = bus.Plan(0x802142, BusAccessType::kRead);
  const auto result = bus.Follow(plan, 0, snes.GetCpu().GetDeviceId());
  REQUIRE(result.data == 0x62);
}

TEST_CASE("APU observes CPU writes on the correct side of an SPC read cycle", "[unit][apu]") {
  SNES before;
  SNES after;
  PreparePortCopy(before.GetApu());
  PreparePortCopy(after.GetApu());
  for (Apu* apu : {&before.GetApu(), &after.GetApu()}) apu->WriteRegister(0x2140, 0x11, 0);

  // MOV A,dp samples on cycle 3 (ceil(3 * 118125 / 5632) = 63).
  before.GetApu().WriteRegister(0x2140, 0x22, 62);
  after.GetApu().WriteRegister(0x2140, 0x22, 63);
  for (Apu* apu : {&before.GetApu(), &after.GetApu()}) {
    apu->CatchUpTo(146);  // Store cycle 7 is still in the future.
    REQUIRE(apu->GetPort(1) == 0);
    apu->CatchUpTo(147);
  }
  REQUIRE(before.GetApu().GetPort(1) == 0x22);
  REQUIRE(after.GetApu().GetPort(1) == 0x11);
}

TEST_CASE("APU IPL overlay preserves underlying RAM and control clears only inputs", "[unit][apu]") {
  SNES snes;
  auto& apu = snes.GetApu();
  apu.Write(0xFFC0, 0x42);
  REQUIRE(apu.PeekRam(0xFFC0) == 0x42);
  REQUIRE(apu.Read(0xFFC0) == 0xCD);
  REQUIRE(apu.Read(0xFFFE) == 0xC0);
  REQUIRE(apu.Read(0xFFFF) == 0xFF);
  for (uint16_t port = 0; port < 4; ++port) {
    apu.WriteRegister(0x2140U + port, 0x21, 0);
    apu.Write(static_cast<uint16_t>(0xF4U + port), 0x75);
  }
  apu.Write(0xF1, 0x10);
  REQUIRE(apu.Read(0xFFC0) == 0x42);
  REQUIRE(apu.Read(0xF1) == 0);
  REQUIRE(apu.PeekRam(0xF1) == 0x10);
  REQUIRE(apu.Read(0xF4) == 0);
  REQUIRE(apu.Read(0xF5) == 0);
  REQUIRE(apu.Read(0xF6) == 0x21);
  REQUIRE(apu.Read(0xF7) == 0x21);
  // CONTROL clears on every write of the bit, including an unchanged value.
  apu.WriteRegister(0x2140, 0x31, 0);
  apu.WriteRegister(0x2141, 0x32, 0);
  apu.Write(0xF1, 0x10);
  REQUIRE(apu.Read(0xF4) == 0);
  REQUIRE(apu.Read(0xF5) == 0);
  apu.Write(0xF1, 0xA0);
  REQUIRE(apu.Read(0xFFC0) == 0xCD);
  REQUIRE(apu.Read(0xF6) == 0);
  REQUIRE(apu.Read(0xF7) == 0);
  for (std::size_t port = 0; port < 4; ++port) REQUIRE(apu.GetPort(port) == 0x75);
}

TEST_CASE("APU accepts default TEST mode and ignores TEST changes with PSW P set", "[unit][apu]") {
  SNES snes;
  auto& apu = snes.GetApu();
  SECTION("default mode") {
    REQUIRE_NOTHROW(apu.Write(0xF0, 0x0A));
    REQUIRE(apu.PeekRam(0xF0) == 0x0A);
  }
  SECTION("P flag disables register effects but preserves underlying ARAM write") {
    Spc700::State state;
    state.psw = 0x20;
    apu.GetCpu().Reset(state);
    REQUIRE_NOTHROW(apu.Write(0xF0, 0xFF));
    REQUIRE(apu.PeekRam(0xF0) == 0xFF);
  }
  REQUIRE(apu.Read(0xF0) == 0);
  REQUIRE_FALSE(apu.GetFault().has_value());
  // TEST has left ordinary RAM accessible and writable.
  apu.Write(0x0200, 0x5A);
  REQUIRE(apu.Read(0x0200) == 0x5A);
}

TEST_CASE("APU reset clears execution, latches, faults and fractional clock state", "[unit][apu]") {
  SNES snes;
  auto& apu = snes.GetApu();
  apu.CatchUpTo(100000);
  apu.Write(0xFFC0, 0x42);
  REQUIRE_THROWS_AS(apu.Write(0xF1, 1), std::runtime_error);
  REQUIRE(apu.GetFault().has_value());
  apu.Reset();
  REQUIRE_FALSE(apu.GetFault().has_value());
  REQUIRE(apu.GetTime() == 0);
  REQUIRE(apu.GetClockPhase() == 0);
  REQUIRE(apu.GetCpu().GetState().cycles == 0);
  REQUIRE(apu.GetCpu().GetState().pc == 0xFFC0);
  REQUIRE(apu.PeekRam(0xFFC0) == 0);
  REQUIRE(apu.Read(0xFFC0) == 0xCD);
  REQUIRE(apu.GetPort(0) == 0);
  apu.CatchUpTo(100000);
  REQUIRE(apu.GetPort(0) == 0xAA);
}

TEST_CASE("APU unsupported hardware stops with persistent diagnostics", "[unit][apu]") {
  SNES snes;
  auto& apu = snes.GetApu();
  SECTION("instruction accesses DSP data") {
    Spc700::State state;
    state.pc = 0x0200;
    apu.Write(0x0200, 0xE4);  // MOV A,$F3: the opcode exists; DSP data does not.
    apu.Write(0x0201, 0xF3);
    apu.GetCpu().Reset(state);
    REQUIRE_THROWS_AS(apu.CatchUpTo(100), std::runtime_error);
    REQUIRE_FALSE(apu.GetCpu().GetState().faulted);
    REQUIRE(apu.GetCpu().GetState().pc == 0x0202);
    REQUIRE(apu.GetTime() == 63);
  }
  SECTION("DSP data") { REQUIRE_THROWS_AS(apu.Read(0xF3), std::runtime_error); }
  SECTION("DSP write") { REQUIRE_THROWS_AS(apu.Write(0xF3, 0x7F), std::runtime_error); }
  SECTION("timer enabling") { REQUIRE_THROWS_AS(apu.Write(0xF1, 0x81), std::runtime_error); }
  SECTION("TEST mode") { REQUIRE_THROWS_AS(apu.Write(0xF0, 0), std::runtime_error); }
  REQUIRE(apu.GetFault().has_value());
  REQUIRE_THROWS_AS(apu.CatchUpTo(1000), std::runtime_error);
}

TEST_CASE("CPU port writes do not wake a sleeping or stopped SPC700", "[unit][apu]") {
  for (const uint8_t opcode : std::array<uint8_t, 2>{0xEF, 0xFF}) {
    CAPTURE(opcode);
    SNES snes;
    auto& apu = snes.GetApu();
    apu.Write(0x0200, opcode);
    apu.GetCpu().Reset(Spc700::State{.pc = 0x0200});
    apu.CatchUpTo(1000);
    const auto cycles = apu.GetCpu().GetState().cycles;
    apu.WriteRegister(0x2140, 0xA5, 2000);
    const auto& state = apu.GetCpu().GetState();
    REQUIRE(state.sleeping == (opcode == 0xEF));
    REQUIRE(state.stopped == (opcode == 0xFF));
    REQUIRE(state.pc == 0x0201);
    REQUIRE(state.cycles > cycles);
    REQUIRE(apu.GetInputPort(0) == 0xA5);
    REQUIRE_FALSE(apu.GetFault().has_value());
    apu.Reset();
    REQUIRE_FALSE(apu.GetCpu().GetState().sleeping);
    REQUIRE_FALSE(apu.GetCpu().GetState().stopped);
  }
}
