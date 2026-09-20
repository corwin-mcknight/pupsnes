#include <array>
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

#include "frontend/audio_panel.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/hw/apu/apu.h"
#include "pupsnes/hw/apu/sdsp.h"
#include "pupsnes/hw/apu/spc_file.h"

namespace {
using pupsnes::SdspBackend;
using pupsnes::SdspMode;
constexpr std::array kBackends = {SdspBackend::kStub, SdspBackend::kThirdParty, SdspBackend::kNative};
using Aram = std::array<uint8_t, 65536>;
}  // namespace

TEST_CASE("DSP implementations preserve registers on load and reset the sample phase", "[unit][apu][sdsp][backend]") {
  for (const auto backend : kBackends) {
    for (const auto mode : {SdspMode::kSimple, SdspMode::kAccurate}) {
      CAPTURE(backend, mode);
      Aram ram{};
      ram.fill(0xAB);
      auto dsp = pupsnes::MakeSdsp(backend, mode, ram.data(), ram.size());
      REQUIRE(dsp->Backend() == backend);
      REQUIRE(dsp->Mode() == mode);
      REQUIRE(dsp->ReadRegister(0x6C) == 0xE0);
      int16_t left = 123;
      int16_t right = -123;
      for (unsigned cycle = 0; cycle < 7; ++cycle) REQUIRE_FALSE(dsp->TickCycle(left, right));
      std::array<uint8_t, 128> registers{};
      registers[0x6C] = 0xE0;
      registers[0x7C] = 0xA5;
      registers[0x0C] = 0x72;
      dsp->LoadRegisters(registers);
      REQUIRE(dsp->ReadRegister(0x7C) == 0xA5);
      REQUIRE(dsp->ReadRegister(0x0C) == 0x72);
      dsp->WriteRegister(0x7C, 0xFF);
      REQUIRE(dsp->ReadRegister(0x7C) == 0);
      for (unsigned cycle = 0; cycle < 31; ++cycle) {
        REQUIRE_FALSE(dsp->TickCycle(left, right));
        REQUIRE(left == 123);
        REQUIRE(right == -123);
      }
      REQUIRE(dsp->TickCycle(left, right));
      REQUIRE(left == 0);
      REQUIRE(right == 0);
      dsp->Reset();
      for (uint8_t index = 0; index < 128; ++index) {
        REQUIRE(dsp->ReadRegister(index) == (index == 0x6C ? 0xE0 : 0));
      }
      Aram expected;
      expected.fill(0xAB);
      REQUIRE(ram == expected);
    }
  }
}

TEST_CASE("Stub and native remain silent and never write echo memory", "[unit][apu][sdsp][backend]") {
  for (const auto backend : {SdspBackend::kStub, SdspBackend::kNative}) {
    Aram ram{};
    ram.fill(0x93);
    const auto expected = ram;
    auto dsp = pupsnes::MakeSdsp(backend, SdspMode::kAccurate, ram.data(), ram.size());
    for (uint8_t index = 0; index < 128; ++index) {
      dsp->WriteRegister(index, 0xFF);
      REQUIRE(dsp->ReadRegister(index) == (index == 0x7C ? 0 : 0xFF));
    }
    dsp->WriteRegister(0x6C, 0);  // Unmuted, echo writes allowed on real hardware.
    for (unsigned sample = 0; sample < 100; ++sample) {
      int16_t left = 32767;
      int16_t right = -32768;
      dsp->StepSample(left, right);
      REQUIRE(left == 0);
      REQUIRE(right == 0);
    }
    REQUIRE(ram == expected);
    REQUIRE(dsp->ReadRegister(0x08) == 0);
    REQUIRE(dsp->ReadRegister(0x09) == 0);
    REQUIRE(dsp->ReadRegister(0x7C) == 0);
  }
}

TEST_CASE("DSP backend changes apply only on reset or SPC load and retain interpolation",
          "[unit][apu][sdsp][backend]") {
  pupsnes::SNES snes;
  REQUIRE(snes.GetSdspBackendLive() == SdspBackend::kThirdParty);
  for (const auto backend : kBackends) {
    const auto before = snes.GetApu().GetDsp().Backend();
    snes.SetSdspBackendPending(backend);
    snes.SetSdspModePending(SdspMode::kSimple);
    REQUIRE(snes.GetApu().GetDsp().Backend() == before);
    snes.Reset();
    REQUIRE(snes.GetSdspBackendLive() == backend);
    REQUIRE(snes.GetApu().GetDsp().Backend() == backend);
    REQUIRE(snes.GetSdspModeLive() == SdspMode::kSimple);
    REQUIRE(snes.GetApu().GetDsp().Mode() == SdspMode::kSimple);
  }
  pupsnes::SpcFile file;
  file.dsp[0x6C] = 0xE0;
  file.dsp[0x7C] = 0x55;
  for (const auto backend : kBackends) {
    snes.SetSdspBackendPending(backend);
    snes.LoadSpc(file);
    REQUIRE(snes.GetSdspBackendLive() == backend);
    REQUIRE(snes.GetApu().GetDsp().Backend() == backend);
    REQUIRE(snes.GetApu().GetDsp().ReadRegister(0x7C) == 0x55);
    // Seeking rebuilds the live backend even if a new selection is pending.
    snes.SetSdspBackendPending(SdspBackend::kThirdParty);
    snes.GetApu().LoadSpc(file, snes.GetSdspModeLive(), snes.GetSdspBackendLive());
    REQUIRE(snes.GetApu().GetDsp().Backend() == backend);
  }
}

TEST_CASE("DSP backend preferences round trip independently of legacy interpolation",
          "[unit][audio][config][backend]") {
  for (const auto backend : kBackends) {
    REQUIRE(pupsnes::frontend::ParseSdspBackend(pupsnes::frontend::SdspBackendSetting(backend)) == backend);
  }
  for (const auto invalid : {"", "0", "2", "native junk", "Native", "third_party"}) {
    REQUIRE_FALSE(pupsnes::frontend::ParseSdspBackend(invalid));
  }
  Aram ram{};
  // Deliberately exercise the factory's invalid-enum rejection paths.
  // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
  REQUIRE_THROWS_AS(pupsnes::MakeSdsp(static_cast<SdspBackend>(255), SdspMode::kAccurate, ram.data(), ram.size()),
                    std::invalid_argument);
  for (const auto backend : kBackends) {
    REQUIRE_THROWS_AS(pupsnes::MakeSdsp(backend, SdspMode::kAccurate, nullptr, ram.size()), std::invalid_argument);
    REQUIRE_THROWS_AS(pupsnes::MakeSdsp(backend, SdspMode::kAccurate, ram.data(), 32), std::invalid_argument);
    REQUIRE_THROWS_AS(pupsnes::MakeSdsp(backend, static_cast<SdspMode>(255), ram.data(), ram.size()),
                      std::invalid_argument);
  }
  // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
}
