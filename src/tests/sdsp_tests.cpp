#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <initializer_list>
#include <memory>

#include "pupsnes/hw/apu/accurate_sdsp.h"
#include "pupsnes/hw/apu/sdsp.h"
#include "pupsnes/hw/apu/simple_sdsp.h"

namespace {

using pupsnes::AccurateSdsp;
using pupsnes::Sdsp;
using pupsnes::SdspMode;
using pupsnes::SimpleSdsp;
using Aram = std::array<uint8_t, 0x10000>;
using Registers = std::array<uint8_t, Sdsp::kRegisterCount>;

std::unique_ptr<Sdsp> MakeDsp(SdspMode mode, Aram& aram) {
  if (mode == SdspMode::kAccurate) return std::make_unique<AccurateSdsp>(aram.data(), aram.size());
  return std::make_unique<SimpleSdsp>(aram.data(), aram.size());
}

Registers ReadBank(const Sdsp& dsp) {
  Registers registers{};
  for (std::size_t index = 0; index < registers.size(); ++index) {
    registers[index] = dsp.ReadRegister(static_cast<uint8_t>(index));
  }
  return registers;
}

Registers ColdBank() {
  Registers registers{};
  registers[0x6C] = 0xE0;
  return registers;
}

}  // namespace

TEST_CASE("Both S-DSP backends start with the deterministic FLG seed and cold reset preserves ARAM",
          "[unit][apu][sdsp]") {
  for (const auto mode : {SdspMode::kSimple, SdspMode::kAccurate}) {
    CAPTURE(mode);
    Aram aram{};
    for (std::size_t address = 0; address < aram.size(); ++address) {
      aram[address] = static_cast<uint8_t>(address ^ (address >> 8));
    }
    const auto original_aram = aram;
    auto dsp = MakeDsp(mode, aram);
    REQUIRE(dsp->Mode() == mode);
    REQUIRE(dsp->ModeName() == (mode == SdspMode::kSimple ? "simple" : "accurate"));
    REQUIRE(ReadBank(*dsp) == ColdBank());
    REQUIRE(aram == original_aram);

    for (std::size_t index = 0; index < Sdsp::kRegisterCount; ++index) {
      dsp->WriteRegister(static_cast<uint8_t>(index), 0xFF);
    }
    dsp->Reset();
    REQUIRE(ReadBank(*dsp) == ColdBank());
    REQUIRE(aram == original_aram);
  }
}

TEST_CASE("S-DSP register writes preserve all eight bits and all 128 addresses remain independent",
          "[unit][apu][sdsp]") {
  for (const auto mode : {SdspMode::kSimple, SdspMode::kAccurate}) {
    CAPTURE(mode);
    Aram aram{};
    auto dsp = MakeDsp(mode, aram);
    Registers expected = ColdBank();
    for (std::size_t index = 0; index < Sdsp::kRegisterCount; ++index) {
      const uint8_t value = static_cast<uint8_t>(index * 37 + 0x53);
      dsp->WriteRegister(static_cast<uint8_t>(index), value);
      expected[index] = index == 0x7C ? 0 : value;
      INFO("Distinct register write: " << index);
      REQUIRE(ReadBank(*dsp) == expected);
    }
    for (const uint8_t value : std::initializer_list<uint8_t>{0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF, 0xA5, 0x5A}) {
      for (std::size_t index = 0; index < Sdsp::kRegisterCount; ++index) {
        CAPTURE(index, value);
        dsp->WriteRegister(static_cast<uint8_t>(index), value);
        REQUIRE(dsp->ReadRegister(static_cast<uint8_t>(index)) == (index == 0x7C ? 0 : value));
      }
    }
  }
}

TEST_CASE("S-DSP processing masks do not discard register readback bits and ENVX OUTX accept CPU writes",
          "[unit][apu][sdsp]") {
  // The DSP uses six PITCHH bits, ignores PMON bit zero for voice zero,
  // and uses four EDL bits, but its raw register bank retains every bit.
  // ENVX/OUTX CPU writes are visible until a later DSP cycle overwrites them.
  for (const auto mode : {SdspMode::kSimple, SdspMode::kAccurate}) {
    CAPTURE(mode);
    Aram aram{};
    auto dsp = MakeDsp(mode, aram);
    for (const uint8_t index : std::initializer_list<uint8_t>{0x03, 0x73, 0x2D, 0x7D, 0x0A, 0x0B, 0x0E, 0x1D}) {
      CAPTURE(index);
      dsp->WriteRegister(index, 0xFF);
      REQUIRE(dsp->ReadRegister(index) == 0xFF);
    }
    for (unsigned voice = 0; voice < 8; ++voice) {
      const auto envx = static_cast<uint8_t>(voice * 0x10 + 8);
      const auto outx = static_cast<uint8_t>(voice * 0x10 + 9);
      dsp->WriteRegister(envx, 0xFF);
      dsp->WriteRegister(outx, 0x81);
      REQUIRE(dsp->ReadRegister(envx) == 0xFF);
      REQUIRE(dsp->ReadRegister(outx) == 0x81);
    }
  }
}

TEST_CASE("Every write to S-DSP ENDX clears it without affecting neighboring registers", "[unit][apu][sdsp]") {
  for (const auto mode : {SdspMode::kSimple, SdspMode::kAccurate}) {
    CAPTURE(mode);
    Aram aram{};
    auto dsp = MakeDsp(mode, aram);
    dsp->WriteRegister(0x7B, 0xA5);
    dsp->WriteRegister(0x7D, 0x5A);
    for (unsigned value = 0; value < 256; ++value) {
      dsp->WriteRegister(0x7C, static_cast<uint8_t>(value));
      REQUIRE(dsp->ReadRegister(0x7C) == 0);
    }
    REQUIRE(dsp->ReadRegister(0x7B) == 0xA5);
    REQUIRE(dsp->ReadRegister(0x7D) == 0x5A);
  }
}

TEST_CASE("Writing S-DSP FLG reset and KON KOFF latches does not clear the register bank", "[unit][apu][sdsp]") {
  for (const auto mode : {SdspMode::kSimple, SdspMode::kAccurate}) {
    CAPTURE(mode);
    Aram aram{};
    auto dsp = MakeDsp(mode, aram);
    dsp->WriteRegister(0x00, 0x56);
    dsp->WriteRegister(0x08, 0x78);
    dsp->WriteRegister(0x09, 0x9A);
    dsp->WriteRegister(0x4C, 0xFF);
    dsp->WriteRegister(0x5C, 0xA5);
    auto expected = ReadBank(*dsp);
    for (const uint8_t flags : std::initializer_list<uint8_t>{0x00, 0x1F, 0x80, 0xE0, 0xFF}) {
      CAPTURE(flags);
      dsp->WriteRegister(0x6C, flags);
      expected[0x6C] = flags;
      REQUIRE(ReadBank(*dsp) == expected);
    }
  }
}

TEST_CASE("Muted reset voices update DSP status without writing echo-disabled ARAM", "[unit][apu][sdsp]") {
  for (const auto mode : {SdspMode::kSimple, SdspMode::kAccurate}) {
    CAPTURE(mode);
    Aram aram{};
    aram.fill(0xA5);
    const auto original_aram = aram;
    auto dsp = MakeDsp(mode, aram);
    for (unsigned voice = 0; voice < 8; ++voice) {
      dsp->WriteRegister(static_cast<uint8_t>(voice * 0x10 + 8), 0x7F);
      dsp->WriteRegister(static_cast<uint8_t>(voice * 0x10 + 9), 0x81);
    }
    // FLG=$E0 keeps voices reset and muted, and inhibits echo writes.
    for (unsigned sample = 0; sample < 64; ++sample) {
      int16_t left = 32767;
      int16_t right = -32768;
      dsp->StepSample(left, right);
      REQUIRE(left == 0);
      REQUIRE(right == 0);
    }
    for (unsigned voice = 0; voice < 8; ++voice) {
      REQUIRE(dsp->ReadRegister(static_cast<uint8_t>(voice * 0x10 + 8)) == 0);
      REQUIRE(dsp->ReadRegister(static_cast<uint8_t>(voice * 0x10 + 9)) == 0);
    }
    REQUIRE(aram == original_aram);
  }
}

TEST_CASE("S-DSP backend instances keep independent register state while matching CPU writes", "[unit][apu][sdsp]") {
  Aram simple_aram{};
  Aram accurate_aram{};
  SimpleSdsp simple(simple_aram.data(), simple_aram.size());
  AccurateSdsp accurate(accurate_aram.data(), accurate_aram.size());
  simple.WriteRegister(0x0C, 0x80);
  REQUIRE(accurate.ReadRegister(0x0C) == 0);
  accurate.WriteRegister(0x0C, 0x55);
  REQUIRE(simple.ReadRegister(0x0C) == 0x80);
  simple.Reset();
  REQUIRE(accurate.ReadRegister(0x0C) == 0x55);
  accurate.Reset();

  for (unsigned step = 0; step < 512; ++step) {
    const auto index = static_cast<uint8_t>((step * 53 + 7) % 128);
    const auto value = static_cast<uint8_t>(step * 19 + 0x63);
    simple.WriteRegister(index, value);
    accurate.WriteRegister(index, value);
    REQUIRE(ReadBank(simple) == ReadBank(accurate));
  }
  simple.Reset();
  accurate.Reset();
  REQUIRE(ReadBank(simple) == ColdBank());
  REQUIRE(ReadBank(accurate) == ColdBank());
}
