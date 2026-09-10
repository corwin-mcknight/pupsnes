#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>

#include "pupsnes/core/snes.h"
#include "pupsnes/hw/apu/apu.h"
#include "pupsnes/hw/apu/sdsp.h"
#include "pupsnes/hw/apu/spc700.h"

namespace {
using pupsnes::Apu;
using pupsnes::SdspMode;
using pupsnes::SNES;
using pupsnes::Spc700;
using pupsnes::TimeMasterT;

constexpr std::array<uint64_t, 3> kTimerPeriods = {128, 128, 16};

constexpr TimeMasterT MasterAtCycle(uint64_t cycle) {
  return static_cast<TimeMasterT>((cycle * Apu::kClockDenominator + Apu::kClockNumerator - 1) / Apu::kClockNumerator);
}

void HaltCpu(Apu& apu, bool sleeping = false) {
  Spc700::State state;
  state.pc = 0x0200;
  state.stopped = !sleeping;
  state.sleeping = sleeping;
  apu.GetCpu().Reset(state);
}

void Advance(Apu& apu, uint64_t cycle) { apu.CatchUpTo(MasterAtCycle(cycle)); }

void StartProgram(Apu& apu, const std::array<uint8_t, 3>& program, uint8_t a = 0) {
  for (std::size_t index = 0; index < program.size(); ++index) {
    apu.Write(static_cast<uint16_t>(0x0200U + index), program[index]);
  }
  Spc700::State state;
  state.pc = 0x0200;
  state.a = a;
  apu.GetCpu().Reset(state);
}
}  // namespace

TEST_CASE("APU timers tick at their exact independent divider edges", "[unit][apu][timer]") {
  SNES snes;
  auto& apu = snes.GetApu();
  HaltCpu(apu);
  for (uint16_t address = 0xFA; address <= 0xFC; ++address) apu.Write(address, 1);
  apu.Write(0xF1, 7);

  apu.CatchUpTo(MasterAtCycle(16) - 1);
  REQUIRE(apu.GetTimerState(2).output == 0);
  REQUIRE(apu.GetTimerState(2).divider == 15);
  Advance(apu, 16);
  REQUIRE(apu.GetTimerState(2).output == 1);
  REQUIRE(apu.GetTimerState(2).divider == 0);
  REQUIRE(apu.GetTimerState(0).output == 0);
  REQUIRE(apu.GetTimerState(1).output == 0);

  apu.CatchUpTo(MasterAtCycle(128) - 1);
  REQUIRE(apu.GetTimerState(0).output == 0);
  REQUIRE(apu.GetTimerState(1).output == 0);
  REQUIRE(apu.GetTimerState(2).output == 7);
  Advance(apu, 128);
  REQUIRE(apu.GetTimerState(0).output == 1);
  REQUIRE(apu.GetTimerState(1).output == 1);
  REQUIRE(apu.GetTimerState(2).output == 8);
  for (std::size_t index = 0; index < 3; ++index) {
    REQUIRE(apu.GetTimerState(index).divider == 0);
    REQUIRE(apu.GetTimerState(index).counter == 0);
  }
}

TEST_CASE("APU timer enabling resets counters only on a rising enable bit", "[unit][apu][timer]") {
  for (std::size_t index = 0; index < 3; ++index) {
    CAPTURE(index);
    SNES snes;
    auto& apu = snes.GetApu();
    HaltCpu(apu);
    const uint64_t period = kTimerPeriods[index];
    const auto enable = static_cast<uint8_t>(1U << index);
    apu.Write(static_cast<uint16_t>(0xFAU + index), 2);
    Advance(apu, period - 3);
    REQUIRE(apu.GetTimerState(index).divider == period - 3);
    REQUIRE(apu.GetTimerState(index).counter == 0);
    apu.Write(0xF1, enable);
    Advance(apu, period);
    REQUIRE(apu.GetTimerState(index).counter == 1);
    Advance(apu, 3 * period + 3);
    REQUIRE(apu.GetTimerState(index).counter == 1);
    REQUIRE(apu.GetTimerState(index).output == 1);
    const auto running = apu.GetTimerState(index);
    apu.Write(0xF1, enable);
    REQUIRE(apu.GetTimerState(index) == running);

    apu.Write(0xF1, 0);
    Advance(apu, 5 * period + 7);
    REQUIRE(apu.GetTimerState(index).divider == 7);
    REQUIRE(apu.GetTimerState(index).counter == 1);
    REQUIRE(apu.GetTimerState(index).output == 1);
    const auto disabled = apu.GetTimerState(index);
    apu.Write(0xF1, 0);
    REQUIRE(apu.GetTimerState(index) == disabled);
    apu.Write(0xF1, enable);
    REQUIRE(apu.GetTimerState(index).enabled);
    REQUIRE(apu.GetTimerState(index).target == 2);
    REQUIRE(apu.GetTimerState(index).divider == 7);
    REQUIRE(apu.GetTimerState(index).counter == 0);
    REQUIRE(apu.GetTimerState(index).output == 0);
    Advance(apu, 6 * period);
    REQUIRE(apu.GetTimerState(index).counter == 1);
    REQUIRE(apu.GetTimerState(index).output == 0);
    Advance(apu, 7 * period);
    REQUIRE(apu.GetTimerState(index).output == 1);
  }
}

TEST_CASE("APU zero timer target counts 256 pulses and outputs wrap at sixteen", "[unit][apu][timer]") {
  for (std::size_t index = 0; index < 3; ++index) {
    CAPTURE(index);
    SNES snes;
    auto& apu = snes.GetApu();
    HaltCpu(apu);
    const uint64_t period = kTimerPeriods[index];
    apu.Write(0xF1, static_cast<uint8_t>(1U << index));
    Advance(apu, 256 * period - 1);
    REQUIRE(apu.GetTimerState(index).counter == 255);
    REQUIRE(apu.GetTimerState(index).output == 0);
    Advance(apu, 256 * period);
    REQUIRE(apu.GetTimerState(index).counter == 0);
    REQUIRE(apu.GetTimerState(index).output == 1);
    Advance(apu, 15 * 256 * period);
    REQUIRE(apu.GetTimerState(index).output == 15);
    Advance(apu, 16 * 256 * period);
    REQUIRE(apu.GetTimerState(index).output == 0);
    Advance(apu, 17 * 256 * period);
    REQUIRE(apu.Read(static_cast<uint16_t>(0xFDU + index)) == 1);
  }
}

TEST_CASE("APU target changes compare after the next eight bit increment", "[unit][apu][timer]") {
  struct Case {
    uint8_t target;
    uint64_t first_output_pulse;
  };
  constexpr std::array<Case, 4> kCases = {{{0, 256}, {1, 257}, {2, 258}, {3, 3}}};
  for (std::size_t index = 0; index < 3; ++index) {
    for (const auto& test : kCases) {
      CAPTURE(index, test.target);
      SNES snes;
      auto& apu = snes.GetApu();
      HaltCpu(apu);
      const uint64_t period = kTimerPeriods[index];
      const auto target_address = static_cast<uint16_t>(0xFAU + index);
      apu.Write(target_address, 4);
      apu.Write(0xF1, static_cast<uint8_t>(1U << index));
      Advance(apu, 2 * period + 1);
      REQUIRE(apu.GetTimerState(index).counter == 2);
      apu.Write(target_address, test.target);
      REQUIRE(apu.GetTimerState(index).counter == 2);
      REQUIRE(apu.GetTimerState(index).output == 0);
      REQUIRE(apu.GetTimerState(index).divider == 1);
      REQUIRE(apu.Read(target_address) == 0);
      REQUIRE(apu.PeekRam(target_address) == test.target);
      Advance(apu, test.first_output_pulse * period - 1);
      REQUIRE(apu.GetTimerState(index).output == 0);
      Advance(apu, test.first_output_pulse * period);
      REQUIRE(apu.GetTimerState(index).counter == 0);
      REQUIRE(apu.GetTimerState(index).output == 1);
    }
  }
}

TEST_CASE("APU output reads clear only the selected four bit counter", "[unit][apu][timer]") {
  SNES snes;
  auto& apu = snes.GetApu();
  HaltCpu(apu);
  for (uint16_t address = 0xFA; address <= 0xFC; ++address) apu.Write(address, 3);
  apu.Write(0xF1, 7);
  Advance(apu, 7 * 128 + 5);
  for (std::size_t index = 0; index < 3; ++index) {
    CAPTURE(index);
    const auto before = apu.GetTimerState(index);
    REQUIRE(before.output == 2);
    const auto address = static_cast<uint16_t>(0xFDU + index);
    apu.Write(address, 0xFF);
    REQUIRE(apu.GetTimerState(index) == before);
    REQUIRE(apu.PeekRam(address) == 0xFF);
    REQUIRE(apu.Read(address) == 2);
    REQUIRE(apu.Read(address) == 0);
    auto cleared = before;
    cleared.output = 0;
    REQUIRE(apu.GetTimerState(index) == cleared);
    REQUIRE(apu.PeekRam(address) == 0xFF);
    for (std::size_t other = index + 1; other < 3; ++other) REQUIRE(apu.GetTimerState(other).output == 2);
  }
  // The partial programmable count survives the reads.
  Advance(apu, 912);
  REQUIRE(apu.GetTimerState(2).output == 1);
  REQUIRE(apu.GetTimerState(0).output == 0);
  REQUIRE(apu.GetTimerState(1).output == 0);
}

TEST_CASE("APU samples timer rollover before an SPC read on the same cycle", "[unit][apu][timer]") {
  SNES snes;
  auto& apu = snes.GetApu();
  HaltCpu(apu);
  apu.Write(0xFC, 1);
  apu.Write(0xF1, 4);
  Advance(apu, 13);
  StartProgram(apu, {0xE4, 0xFF, 0xFF});  // MOV A,$FF; STOP.
  apu.CatchUpTo(MasterAtCycle(16) - 1);
  REQUIRE(apu.GetCpu().GetState().a == 0);
  REQUIRE(apu.GetTimerState(2).output == 0);
  Advance(apu, 16);
  REQUIRE(apu.GetCpu().GetState().a == 1);
  REQUIRE(apu.GetTimerState(2).output == 0);
}

TEST_CASE("APU timer edges precede SPC enable and target writes", "[unit][apu][timer]") {
  SNES snes;
  auto& apu = snes.GetApu();
  HaltCpu(apu);
  SECTION("enabling at an edge cannot count that disabled pulse") {
    apu.Write(0xFC, 1);
    Advance(apu, 11);
    StartProgram(apu, {0x8F, 0x04, 0xF1});  // MOV $F1,#$04 writes on cycle 5.
    Advance(apu, 16);
    REQUIRE(apu.GetTimerState(2).enabled);
    REQUIRE(apu.GetTimerState(2).output == 0);
    REQUIRE(apu.GetTimerState(2).divider == 0);
    HaltCpu(apu);
    Advance(apu, 32);
    REQUIRE(apu.GetTimerState(2).output == 1);
  }
  SECTION("disabling at an edge retains the pulse that just completed") {
    apu.Write(0xFC, 1);
    apu.Write(0xF1, 4);
    Advance(apu, 11);
    StartProgram(apu, {0x8F, 0x00, 0xF1});
    Advance(apu, 16);
    REQUIRE_FALSE(apu.GetTimerState(2).enabled);
    REQUIRE(apu.GetTimerState(2).output == 1);
    HaltCpu(apu);
    Advance(apu, 32);
    REQUIRE(apu.GetTimerState(2).output == 1);
  }
  SECTION("an edge compares the old target before replacing it") {
    apu.Write(0xFC, 1);
    apu.Write(0xF1, 4);
    Advance(apu, 11);
    StartProgram(apu, {0x8F, 0x02, 0xFC});
    Advance(apu, 16);
    REQUIRE(apu.GetTimerState(2).target == 2);
    REQUIRE(apu.GetTimerState(2).counter == 0);
    REQUIRE(apu.GetTimerState(2).output == 1);
    HaltCpu(apu);
    Advance(apu, 32);
    REQUIRE(apu.GetTimerState(2).counter == 1);
    REQUIRE(apu.GetTimerState(2).output == 1);
    Advance(apu, 48);
    REQUIRE(apu.GetTimerState(2).output == 2);
  }
}

TEST_CASE("APU timer output observes store dummy reads but ignores writes", "[unit][apu][timer]") {
  for (const std::size_t index : {std::size_t{0}, std::size_t{2}}) {
    for (const bool direct_copy : {false, true}) {
      CAPTURE(index, direct_copy);
      SNES snes;
      auto& apu = snes.GetApu();
      HaltCpu(apu);
      const auto output = static_cast<uint8_t>(0xFDU + index);
      apu.Write(static_cast<uint16_t>(0xFAU + index), 1);
      apu.Write(0xF1, static_cast<uint8_t>(1U << index));
      Advance(apu, kTimerPeriods[index]);
      apu.Write(0xF1, 0);
      REQUIRE(apu.GetTimerState(index).output == 1);
      apu.Write(0x40, 0xA5);
      if (direct_copy) {
        StartProgram(apu, {0xFA, 0x40, output});  // MOV dp,dp has no destination read.
      } else {
        StartProgram(apu, {0xC4, output, 0xFF}, 0xA5);  // MOV dp,A dummy-reads on cycle 3.
      }
      Advance(apu, kTimerPeriods[index] + 2);
      REQUIRE(apu.GetTimerState(index).output == 1);
      Advance(apu, kTimerPeriods[index] + 3);
      REQUIRE(apu.GetTimerState(index).output == (direct_copy ? 1 : 0));
      REQUIRE(apu.PeekRam(output) == 0);
      Advance(apu, kTimerPeriods[index] + (direct_copy ? 5 : 4));
      REQUIRE(apu.PeekRam(output) == 0xA5);
      REQUIRE(apu.GetTimerState(index).output == (direct_copy ? 1 : 0));
    }
  }
}

TEST_CASE("APU DSP clocks continue through both CPU halt states", "[unit][apu][dsp][timer]") {
  for (const auto mode : {SdspMode::kSimple, SdspMode::kAccurate}) {
    for (const bool sleeping : {false, true}) {
      CAPTURE(static_cast<int>(mode), sleeping);
      SNES snes;
      auto& apu = snes.GetApu();
      apu.Reset(mode);
      HaltCpu(apu, sleeping);
      apu.Write(0xFC, 2);
      apu.Write(0xF1, 4);
      apu.Write(0xF2, 0x0C);
      apu.Write(0xF3, 0x57);
      const auto ram = apu.GetRam();
      Advance(apu, 31);
      REQUIRE(apu.GetDspClockPhase() == 31);
      REQUIRE(apu.GetDspSampleCount() == 0);
      REQUIRE(apu.GetTimerState(2).output == 0);
      apu.CatchUpTo(MasterAtCycle(32) - 1);
      REQUIRE(apu.GetDspSampleCount() == 0);
      Advance(apu, 32);
      REQUIRE(apu.GetDspClockPhase() == 0);
      REQUIRE(apu.GetDspSampleCount() == 1);
      REQUIRE(apu.GetTimerState(2).output == 1);
      Advance(apu, 65);
      REQUIRE(apu.GetDspClockPhase() == 1);
      REQUIRE(apu.GetDspSampleCount() == 2);
      REQUIRE(apu.GetCpu().GetState().cycles == 65);
      REQUIRE(apu.GetCpu().GetState().sleeping == sleeping);
      REQUIRE(apu.GetCpu().GetState().stopped == !sleeping);
      REQUIRE(apu.GetDsp().ReadRegister(0x0C) == 0x57);
      REQUIRE(apu.GetRam() == ram);
    }
  }
}

TEST_CASE("APU cold reset clears timer and DSP state including clock phases", "[unit][apu][dsp][timer]") {
  for (const auto mode : {SdspMode::kSimple, SdspMode::kAccurate}) {
    CAPTURE(static_cast<int>(mode));
    SNES snes;
    auto& apu = snes.GetApu();
    apu.Reset(mode);
    HaltCpu(apu);
    for (uint16_t address = 0xFA; address <= 0xFC; ++address) apu.Write(address, 3);
    apu.Write(0xF1, 7);
    apu.Write(0xF2, 0x0C);
    apu.Write(0xF3, 0x57);
    Advance(apu, 7 * 128 + 5);
    REQUIRE(apu.GetDspSampleCount() > 0);
    REQUIRE(apu.GetDspClockPhase() == 5);
    REQUIRE(apu.GetClockPhase() != 0);
    for (std::size_t index = 0; index < 3; ++index) {
      REQUIRE(apu.GetTimerState(index).output != 0);
      REQUIRE(apu.GetTimerState(index).counter != 0);
    }
    apu.Reset(mode);
    REQUIRE(apu.GetDsp().Mode() == mode);
    REQUIRE(apu.GetDspSampleCount() == 0);
    REQUIRE(apu.GetDspClockPhase() == 0);
    REQUIRE(apu.GetClockPhase() == 0);
    REQUIRE(apu.GetTime() == 0);
    REQUIRE(apu.GetCpu().GetState().cycles == 0);
    REQUIRE(apu.GetCpu().GetState().pc == 0xFFC0);
    REQUIRE(apu.Read(0xF2) == 0);
    for (std::size_t index = 0; index < 3; ++index) REQUIRE(apu.GetTimerState(index) == Apu::TimerState{});
    for (uint8_t index = 0; index < 0x80; ++index) {
      REQUIRE(apu.GetDsp().ReadRegister(index) == (index == 0x6C ? 0xE0 : 0));
    }
    for (const auto value : apu.GetRam()) REQUIRE(value == 0);
    HaltCpu(apu);
    apu.Write(0xFC, 1);
    apu.Write(0xF1, 4);
    Advance(apu, 15);
    REQUIRE(apu.GetTimerState(2).output == 0);
    Advance(apu, 16);
    REQUIRE(apu.GetTimerState(2).output == 1);
  }
}

TEST_CASE("APU timer and DSP evolution is invariant under master clock slicing", "[unit][apu][dsp][timer]") {
  for (const auto mode : {SdspMode::kSimple, SdspMode::kAccurate}) {
    CAPTURE(static_cast<int>(mode));
    SNES whole;
    SNES sliced;
    auto& expected = whole.GetApu();
    auto& actual = sliced.GetApu();
    for (Apu* apu : {&expected, &actual}) {
      apu->Reset(mode);
      HaltCpu(*apu, true);
      apu->Write(0xFA, 0);
      apu->Write(0xFB, 3);
      apu->Write(0xFC, 7);
      apu->Write(0xF1, 7);
      apu->Write(0xF2, 0x0C);
      apu->Write(0xF3, 0x75);
    }
    constexpr std::array<TimeMasterT, 4> kCheckpoints = {1001, 15000, 50003, 800009};
    for (std::size_t step = 0; step < kCheckpoints.size(); ++step) {
      const auto target = kCheckpoints[step];
      expected.CatchUpTo(target);
      for (TimeMasterT time = actual.GetTime(); time < target;) {
        const TimeMasterT stride = step == 0 ? 1 : (time % 137) + 1;
        time = time + stride < target ? time + stride : target;
        actual.CatchUpTo(time);
      }
      REQUIRE(actual.GetTime() == expected.GetTime());
      REQUIRE(actual.GetClockPhase() == expected.GetClockPhase());
      REQUIRE(actual.GetDspClockPhase() == expected.GetDspClockPhase());
      REQUIRE(actual.GetDspSampleCount() == expected.GetDspSampleCount());
      REQUIRE(actual.GetCpu().GetState().cycles == expected.GetCpu().GetState().cycles);
      REQUIRE(actual.GetCpu().GetState().pc == expected.GetCpu().GetState().pc);
      REQUIRE(actual.GetRam() == expected.GetRam());
      for (std::size_t index = 0; index < 3; ++index) {
        REQUIRE(actual.GetTimerState(index) == expected.GetTimerState(index));
      }
      for (uint8_t index = 0; index < 0x80; ++index) {
        REQUIRE(actual.GetDsp().ReadRegister(index) == expected.GetDsp().ReadRegister(index));
      }
      // Identical externally timed MMIO changes include counter reads, target
      // replacement, and disabling/re-enabling all three timers between slices.
      REQUIRE(actual.Read(0xFF) == expected.Read(0xFF));
      const auto control = static_cast<uint8_t>(step == 1 ? 0 : 7);
      for (Apu* apu : {&expected, &actual}) {
        apu->Write(0xFC, static_cast<uint8_t>(step + 1));
        apu->Write(0xF1, control);
      }
    }
  }
}
