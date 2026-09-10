#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

#include "pupsnes/core/snes.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/apu/apu.h"
#include "pupsnes/memory/wram.h"
#include "pupsnes/tools/trace_runner.h"

namespace {
using pupsnes::Apu;
using pupsnes::SdspMode;
using pupsnes::SNES;
using pupsnes::TimeMasterT;
using Sample = std::array<int16_t, 2>;
constexpr TimeMasterT kBudget = 1'000'000;

TimeMasterT MasterAtSpcCycle(uint64_t cycle) {
  return (cycle * Apu::kClockDenominator + Apu::kClockNumerator - 1) / Apu::kClockNumerator;
}

std::vector<uint8_t> AudioRom() {
  const auto path = std::filesystem::path(PUPSNES_TEST_ROM_DIR) / "apu_audio.sfc";
  std::ifstream input(path, std::ios::binary);
  REQUIRE(input.good());
  return {std::istreambuf_iterator<char>(input), {}};
}

struct AudioSnapshot {
  std::vector<Sample> pcm;
  std::array<uint8_t, Apu::kRamSize> ram{};
  std::array<uint8_t, 128> dsp{};
  std::array<uint64_t, 8> spc{};
};

AudioSnapshot Capture(std::span<const uint8_t> rom, SdspMode mode, std::span<const TimeMasterT> slices) {
  SNES snes;
  snes.SetSdspModePending(mode);
  REQUIRE(snes.LoadRom(rom).ok);
  snes.Reset();
  AudioSnapshot snapshot;
  snes.SetAudioSampleCallback([&](int16_t left, int16_t right) { snapshot.pcm.push_back({left, right}); });
  std::size_t slice = 0;
  while (snes.GetMasterTime() < kBudget) {
    const auto target = std::min(kBudget, snes.GetMasterTime() + slices[slice++ % slices.size()]);
    const auto error = pupsnes::tools::DriveMachineToMasterTime(snes, target);
    INFO(error.value_or(""));
    REQUIRE_FALSE(error.has_value());
  }
  REQUIRE_FALSE(snes.GetCpu().GetFault().has_value());
  REQUIRE_FALSE(snes.GetApu().GetFault().has_value());
  REQUIRE(snes.GetCpu().GetHaltState() == pupsnes::HaltState::kStp);
  REQUIRE(snes.GetApu().GetCpu().GetState().stopped);
  constexpr std::array<uint8_t, 5> kReply = {'P', 'A', 'S', 'S', 0x38};
  for (uint32_t address = 0; address < kReply.size(); ++address) {
    REQUIRE(snes.GetWram().Peek(address) == kReply[address]);
  }
  const auto& apu = snes.GetApu();
  const auto& state = apu.GetCpu().GetState();
  snapshot.ram = apu.GetRam();
  for (uint8_t index = 0; index < 128; ++index) snapshot.dsp[index] = apu.GetDsp().ReadRegister(index);
  snapshot.spc = {state.a, state.x, state.y, state.sp, state.pc, state.psw, state.cycles, apu.GetDspSampleCount()};
  REQUIRE(snapshot.pcm.size() == (kBudget * Apu::kClockNumerator / Apu::kClockDenominator) / 32);
  snes.SetAudioSampleCallback({});
  return snapshot;
}
}  // namespace

TEST_CASE("The APU delivers native stereo samples at exact master timestamps and preserves the callback on reset",
          "[unit][apu][audio]") {
  SNES snes;
  auto& apu = snes.GetApu();
  std::vector<TimeMasterT> timestamps;
  std::vector<Sample> pcm;
  snes.SetAudioSampleCallback([&](int16_t left, int16_t right) {
    timestamps.push_back(apu.GetTime());
    pcm.push_back({left, right});
  });
  apu.CatchUpTo(MasterAtSpcCycle(32) - 1);
  REQUIRE(pcm.empty());
  apu.CatchUpTo(MasterAtSpcCycle(96));
  const std::vector<TimeMasterT> expected = {MasterAtSpcCycle(32), MasterAtSpcCycle(64), MasterAtSpcCycle(96)};
  REQUIRE(timestamps == expected);
  REQUIRE(std::ranges::all_of(pcm, [](Sample sample) { return sample == Sample{}; }));
  snes.Reset();
  apu.CatchUpTo(MasterAtSpcCycle(32));
  REQUIRE(pcm.size() == 4);
  REQUIRE(timestamps.back() == MasterAtSpcCycle(32));
  snes.SetAudioSampleCallback({});
  apu.CatchUpTo(MasterAtSpcCycle(64));
  REQUIRE(pcm.size() == 4);
  REQUIRE(apu.GetDspSampleCount() == 2);
}

TEST_CASE("DSP echo writes reach underlying ARAM without changing auxiliary I/O latches", "[unit][apu][audio]") {
  for (const auto mode : {SdspMode::kSimple, SdspMode::kAccurate}) {
    SNES snes;
    auto& apu = snes.GetApu();
    apu.Reset(mode);
    apu.GetCpu().Reset(pupsnes::Spc700::State{.pc = 0x200, .stopped = true});
    apu.Write(0xF8, 0xA5);
    apu.Write(0xF9, 0x5A);
    apu.Write(0xF2, 0x7D);
    apu.Write(0xF3, 1);  // Echo region $0000-$07FF includes the MMIO shadow.
    apu.Write(0xF2, 0x6C);
    apu.Write(0xF3, 0x40);  // Muted voices, with echo writes enabled.
    apu.CatchUpTo(MasterAtSpcCycle(32 * 128));
    REQUIRE(apu.PeekRam(0xF8) == 0);
    REQUIRE(apu.PeekRam(0xF9) == 0);
    REQUIRE(apu.Read(0xF8) == 0xA5);
    REQUIRE(apu.Read(0xF9) == 0x5A);
    apu.Reset(mode);
    REQUIRE(apu.Read(0xF8) == 0);
    REQUIRE(apu.Read(0xF9) == 0);
  }
}

TEST_CASE("A real IPL upload plays a periodic BRR tone through the SNES audio callback",
          "[integration][rom][apu][audio]") {
  const auto rom = AudioRom();
  for (const auto mode : {SdspMode::kSimple, SdspMode::kAccurate}) {
    CAPTURE(static_cast<unsigned>(mode));
    const auto captured = Capture(rom, mode, std::array<TimeMasterT, 1>{kBudget});
    REQUIRE(captured.pcm.size() > 512);
    const auto start = captured.pcm.size() - 512;
    int16_t peak = 0;
    int16_t trough = 0;
    bool stereo_differs = false;
    for (std::size_t frame = start; frame < captured.pcm.size(); ++frame) {
      peak = std::max(peak, captured.pcm[frame][0]);
      trough = std::min(trough, captured.pcm[frame][0]);
      stereo_differs |= captured.pcm[frame][0] != captured.pcm[frame][1];
      if (frame >= start + 64) REQUIRE(captured.pcm[frame] == captured.pcm[frame - 64]);
    }
    REQUIRE(peak > 1000);
    REQUIRE(trough < -1000);
    REQUIRE(stereo_differs);
    REQUIRE((captured.dsp[0x7C] & 1U) != 0);  // The looping BRR end flag reached ENDX.
    REQUIRE(captured.dsp[0x08] == 0x7F);      // Direct GAIN is visible in ENVX.
  }
}

TEST_CASE("An uploaded tone with key-on removed acknowledges the CPU but generates no audio",
          "[integration][rom][apu][audio]") {
  auto rom = AudioRom();
  constexpr std::size_t kKeyOnValue = 0x0100 + 0x00F0 + 4;
  REQUIRE(rom[kKeyOnValue - 1] == 0x8F);
  REQUIRE(rom[kKeyOnValue] == 1);
  REQUIRE(rom[kKeyOnValue + 1] == 0xF3);
  rom[kKeyOnValue] = 0;
  const auto captured = Capture(rom, SdspMode::kAccurate, std::array<TimeMasterT, 1>{kBudget});
  REQUIRE(std::ranges::all_of(captured.pcm, [](Sample sample) { return sample == Sample{}; }));
}

TEST_CASE("Uploaded audio PCM and hardware state are identical across execution slices",
          "[integration][rom][apu][audio]") {
  const auto rom = AudioRom();
  for (const auto mode : {SdspMode::kSimple, SdspMode::kAccurate}) {
    CAPTURE(static_cast<unsigned>(mode));
    const auto expected = Capture(rom, mode, std::array<TimeMasterT, 1>{kBudget});
    const std::array<TimeMasterT, 9> irregular = {1, 19, 4079, 2, 127, 47, 5, 673, 31};
    const auto actual = Capture(rom, mode, irregular);
    REQUIRE(actual.pcm == expected.pcm);
    REQUIRE(actual.ram == expected.ram);
    REQUIRE(actual.dsp == expected.dsp);
    REQUIRE(actual.spc == expected.spc);
  }
}
