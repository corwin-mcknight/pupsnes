#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "pupsnes/core/snes.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/apu/apu.h"
#include "pupsnes/hw/apu/spc_file.h"

namespace {
std::vector<uint8_t> Snapshot() {
  std::vector<uint8_t> bytes(0x10200);
  constexpr std::string_view kHeader = "SNES-SPC700 Sound File Data v0.30";
  std::copy(kHeader.begin(), kHeader.end(), bytes.begin());
  bytes[0x21] = bytes[0x22] = bytes[0x23] = 0x1A;
  bytes[0x24] = 30;
  bytes[0x26] = 2;  // PC = $0200.
  bytes[0x27] = 0x12;
  bytes[0x28] = 0x34;
  bytes[0x29] = 0x56;
  bytes[0x2A] = 0x78;
  bytes[0x2B] = 0xEF;
  bytes[0x300] = 0x2F;  // BRA $0200 forever; no host CPU needed.
  bytes[0x301] = 0xFE;
  bytes[0x1016C] = 0xE0;  // Silent DSP.
  return bytes;
}

pupsnes::TimeMasterT MasterAtCycle(uint64_t cycle) {
  return (cycle * pupsnes::Apu::kClockDenominator + pupsnes::Apu::kClockNumerator - 1) / pupsnes::Apu::kClockNumerator;
}

pupsnes::SpcFile Tone() {
  auto file = pupsnes::SpcFile::Parse(Snapshot());
  // One looping BRR block, direct gain, two different channel volumes.
  file.ram[0x2000] = file.ram[0x2002] = 0;
  file.ram[0x2001] = file.ram[0x2003] = 0x30;
  constexpr std::array<uint8_t, 9> kBrr = {0x93, 0x12, 0x37, 0x6A, 0xCE, 0xFD, 0xB9, 0x80, 0x54};
  std::copy(kBrr.begin(), kBrr.end(), file.ram.begin() + 0x3000);
  file.dsp[0] = 0x60;
  file.dsp[1] = 0x40;
  file.dsp[3] = 0x10;
  file.dsp[7] = 0x7F;
  file.dsp[0x0C] = file.dsp[0x1C] = 0x70;
  file.dsp[0x4C] = 1;
  file.dsp[0x5D] = 0x20;
  file.dsp[0x6C] = 0x20;
  return file;
}
}  // namespace

TEST_CASE("SPC files validate the header and all required hardware data", "[unit][spc]") {
  auto bytes = Snapshot();
  for (const auto length : {0U, 32U, 0x100U, 0x10100U, 0x1017FU}) {
    REQUIRE_THROWS_AS(pupsnes::SpcFile::Parse(std::span(bytes).first(length)), std::invalid_argument);
  }
  REQUIRE_NOTHROW(pupsnes::SpcFile::Parse(std::span(bytes).first(0x10180)));
  for (const auto offset : {0U, 0x21U, 0x22U, 0x23U}) {
    auto corrupt = bytes;
    corrupt[offset] = 0;
    REQUIRE_THROWS_AS(pupsnes::SpcFile::Parse(corrupt), std::invalid_argument);
  }
  constexpr std::string_view kTitle = "Test track  ";
  std::copy(kTitle.begin(), kTitle.end(), bytes.begin() + 0x2E);
  const auto file = pupsnes::SpcFile::Parse(bytes);
  REQUIRE(file.title == "Test track");
  REQUIRE(file.cpu.pc == 0x200);
  REQUIRE(file.cpu.a == 0x12);
  REQUIRE(file.cpu.x == 0x34);
  REQUIRE(file.cpu.y == 0x56);
  REQUIRE(file.cpu.psw == 0x78);
  REQUIRE(file.cpu.sp == 0xEF);
  bytes[0x23] = 0x1B;
  REQUIRE(pupsnes::SpcFile::Parse(bytes).title.empty());
}

TEST_CASE("SPC extra RAM is restored only beneath an enabled IPL ROM", "[unit][spc][apu]") {
  auto bytes = Snapshot();
  bytes[0x100C0] = 0x12;
  bytes[0x101C0] = 0x34;
  REQUIRE(pupsnes::SpcFile::Parse(bytes).ram[0xFFC0] == 0x12);
  bytes[0x1F1] = 0x80;
  const auto file = pupsnes::SpcFile::Parse(bytes);
  REQUIRE(file.ram[0xFFC0] == 0x34);
  pupsnes::SNES snes;
  snes.GetApu().LoadSpc(file);
  REQUIRE(snes.GetApu().Read(0xFFC0) == 0xCD);
  snes.GetApu().Write(0xF1, 0);
  REQUIRE(snes.GetApu().Read(0xFFC0) == 0x34);
  REQUIRE(pupsnes::SpcFile::Parse(std::span(bytes).first(0x10180)).ram[0xFFC0] == 0x12);
}

TEST_CASE("SPC loading restores registers without CONTROL or DSP write side effects", "[unit][spc][apu]") {
  auto file = pupsnes::SpcFile::Parse(Snapshot());
  file.ram[0xF1] = 0x37;  // Timer enables and port-clear bits.
  file.ram[0xF2] = 0x7C;
  file.dsp[0x7C] = 0xA5;  // ENDX would be cleared by an ordinary write.
  for (unsigned i = 0; i < 4; ++i) file.ram[0xF4 + i] = static_cast<uint8_t>(0x60 + i);
  file.ram[0xF8] = 0xAA;
  file.ram[0xFA] = 1;
  file.ram[0xFD] = 0xBE;
  pupsnes::SNES snes;
  auto& apu = snes.GetApu();
  apu.LoadSpc(file);
  REQUIRE(apu.Read(0xF3) == 0xA5);
  for (unsigned i = 0; i < 4; ++i) {
    REQUIRE(apu.GetPort(i) == 0x60 + i);
    REQUIRE(apu.GetInputPort(i) == 0x60 + i);
  }
  REQUIRE(apu.Read(0xF8) == 0xAA);
  REQUIRE(apu.Read(0xFD) == 0xE);
  REQUIRE(apu.Read(0xFD) == 0);
  apu.CatchUpTo(MasterAtCycle(1));
  REQUIRE(apu.Read(0xFD) == 1);
  apu.CatchUpTo(MasterAtCycle(128));
  REQUIRE(apu.Read(0xFD) == 0);
  REQUIRE(apu.GetCpu().GetState().cycles == 128);
}

TEST_CASE("SPC playback produces stereo audio and restarts identically across time slices", "[unit][spc][audio]") {
  for (const auto mode : {pupsnes::SdspMode::kAccurate, pupsnes::SdspMode::kSimple}) {
    pupsnes::SNES snes;
    const auto file = Tone();
    std::vector<std::array<int16_t, 2>> pcm;
    snes.SetAudioSampleCallback([&](int16_t left, int16_t right) { pcm.push_back({left, right}); });
    snes.GetApu().LoadSpc(file, mode);
    const auto target = MasterAtCycle(32 * 1024);
    snes.GetApu().CatchUpTo(target);
    REQUIRE(pcm.size() == 1024);
    REQUIRE(std::ranges::any_of(pcm, [](auto sample) { return sample[0] > 1000; }));
    REQUIRE(std::ranges::any_of(pcm, [](auto sample) { return sample[0] < -1000; }));
    REQUIRE(std::ranges::any_of(pcm, [](auto sample) { return sample[0] != sample[1]; }));
    const auto expected = pcm;
    pcm.clear();
    snes.GetApu().LoadSpc(file, mode);
    for (pupsnes::TimeMasterT t = 1; t < target; t += 137) snes.GetApu().CatchUpTo(t);
    snes.GetApu().CatchUpTo(target);
    REQUIRE(pcm == expected);
    // Standalone playback never advances the main machine clock.
    REQUIRE(snes.GetMasterTime() == 0);
    snes.SetAudioSampleCallback({});
  }
}

TEST_CASE("SPC playback timing reads text and binary ID666", "[unit][spc]") {
  auto bytes = Snapshot();
  bytes[0xA9] = '4';
  bytes[0xAC] = '5';
  bytes[0xAD] = '0';
  bytes[0xAE] = '0';
  auto file = pupsnes::SpcFile::Parse(bytes);
  REQUIRE(file.play_frames == 128000);
  REQUIRE(file.fade_frames == 16000);
  REQUIRE(file.EndFrame() == 144000);
  REQUIRE_FALSE(file.Finished(143999));
  REQUIRE(file.Finished(144000));
  REQUIRE(file.FadeSample(10000, 127999) == 10000);
  REQUIRE(file.FadeSample(10000, 136000) == 5000);
  REQUIRE(file.FadeSample(-10000, 136000) == -5000);
  REQUIRE(file.FadeSample(10000, 144000) == 0);
  bytes[0xA9] = 4;
  bytes[0xAC] = 0xF4;
  bytes[0xAD] = 1;
  bytes[0xAE] = 0;
  file = pupsnes::SpcFile::Parse(bytes);
  REQUIRE(file.play_frames == 128000);
  REQUIRE(file.fade_frames == 16000);
  file.fade_frames = 0;
  REQUIRE(file.FadeSample(10000, 128000) == 0);
  REQUIRE(file.Finished(128000));
  file = pupsnes::SpcFile::Parse(Snapshot());
  REQUIRE_FALSE(file.EndFrame());
  REQUIRE_FALSE(file.Finished(1000000));
  REQUIRE(file.FadeSample(10000, 1000000) == 10000);
}

TEST_CASE("SPC xid6 timing overrides ID666 and validates chunk bounds", "[unit][spc]") {
  auto bytes = Snapshot();
  bytes[0xA9] = '9';
  // Intro 2 s + three loops of 1 s - end adjustment 0.5 s; fade 0.5 s.
  const std::vector<uint8_t> tags = {'x',  'i',  'd',  '6', 36, 0,    0,    0,    0x30, 4,    4,    0, 0x00, 0xF4, 0x01,
                                     0x00, 0x31, 4,    4,   0,  0x00, 0xFA, 0x00, 0x00, 0x32, 4,    4, 0,    0x00, 0x83,
                                     0xFF, 0xFF, 0x33, 4,   4,  0,    0x00, 0x7D, 0x00, 0x00, 0x35, 0, 3,    0};
  bytes.insert(bytes.end(), tags.begin(), tags.end());
  const auto file = pupsnes::SpcFile::Parse(bytes);
  REQUIRE(file.play_frames == 144000);
  REQUIRE(file.fade_frames == 16000);
  REQUIRE(file.EndFrame() == 160000);
  bytes.pop_back();
  REQUIRE_THROWS_AS(pupsnes::SpcFile::Parse(bytes), std::invalid_argument);
  bytes[0x10204] = 35;  // Chunk itself fits, final item doesn't.
  REQUIRE_THROWS_AS(pupsnes::SpcFile::Parse(bytes), std::invalid_argument);
  bytes[0x1020A] = 255;  // First payload extends beyond the chunk.
  REQUIRE_THROWS_AS(pupsnes::SpcFile::Parse(bytes), std::invalid_argument);
}

TEST_CASE("Standalone SPC load leaves the main CPU untouched and replay restores seek state", "[unit][spc]") {
  pupsnes::SNES snes;
  snes.GetCpu().DebugInjectFault(0xFF, 0x123456);
  snes.SetSdspModePending(pupsnes::SdspMode::kSimple);
  const auto file = Tone();
  snes.LoadSpc(file);
  REQUIRE(snes.GetCpu().GetFault().has_value());  // A main CPU reset would clear this.
  REQUIRE(snes.GetSdspModeLive() == pupsnes::SdspMode::kSimple);
  std::vector<std::array<int16_t, 2>> pcm;
  snes.SetAudioSampleCallback([&](int16_t left, int16_t right) { pcm.push_back({left, right}); });
  snes.GetApu().CatchUpTo(MasterAtCycle(32 * 2000));
  const std::vector<std::array<int16_t, 2>> expected(pcm.begin() + 1000, pcm.end());
  // Backward seeking reloads, silently replays to the destination, then resumes.
  snes.LoadSpc(file);
  REQUIRE(snes.GetMasterTime() == 0);
  REQUIRE(snes.GetApu().GetTime() == 0);
  snes.SetAudioSampleCallback({});
  snes.GetApu().CatchUpTo(MasterAtCycle(32 * 1000));
  pcm.clear();
  snes.SetAudioSampleCallback([&](int16_t left, int16_t right) { pcm.push_back({left, right}); });
  snes.GetApu().CatchUpTo(MasterAtCycle(32 * 2000));
  REQUIRE(pcm == expected);
  snes.SetAudioSampleCallback({});
}

TEST_CASE("SPC startup does not play stale samples from writable echo memory", "[unit][spc][audio]") {
  auto file = pupsnes::SpcFile::Parse(Snapshot());
  file.dsp[0x6C] = 0;  // Echo writes enabled, no keyed voices.
  file.dsp[0x6D] = 0x60;
  file.dsp[0x7D] = 1;
  file.dsp[0x2C] = file.dsp[0x3C] = 127;
  file.dsp[0x7F] = 127;
  file.dsp[0x0D] = 64;
  std::fill_n(file.ram.begin() + 0x6000, 0x800, 0x7F);
  for (const auto mode : {pupsnes::SdspMode::kAccurate, pupsnes::SdspMode::kSimple}) {
    pupsnes::SNES snes;
    snes.GetApu().LoadSpc(file, mode);
    std::vector<std::array<int16_t, 2>> pcm;
    snes.SetAudioSampleCallback([&](int16_t left, int16_t right) { pcm.push_back({left, right}); });
    snes.GetApu().CatchUpTo(MasterAtCycle(32 * 1024));
    REQUIRE(pcm.size() == 1024);
    REQUIRE(std::ranges::all_of(pcm, [](auto sample) { return sample[0] == 0 && sample[1] == 0; }));
    snes.SetAudioSampleCallback({});
  }
}

TEST_CASE("SPC echo initialization respects write disable, address wrapping and zero delay", "[unit][spc]") {
  auto file = pupsnes::SpcFile::Parse(Snapshot());
  file.dsp[0x6D] = 0xFF;
  file.dsp[0x7D] = 1;
  file.ram.fill(0xA5);
  file.ram[0xF1] = 0;
  pupsnes::SNES snes;
  snes.GetApu().LoadSpc(file);
  REQUIRE(snes.GetApu().PeekRam(0xFF00) == 0xA5);  // FLG=$E0 preserves read-only echo/data.
  file.dsp[0x6C] = 0;
  snes.GetApu().LoadSpc(file);
  REQUIRE(snes.GetApu().PeekRam(0xFEFF) == 0xA5);
  REQUIRE(snes.GetApu().PeekRam(0xFF00) == 0);
  REQUIRE(snes.GetApu().PeekRam(0x06FF) == 0);
  REQUIRE(snes.GetApu().PeekRam(0x0700) == 0xA5);
  REQUIRE(snes.GetApu().GetInputPort(0) == 0xA5);  // Clearing ARAM is not an I/O write.
  file.dsp[0x7D] = 0;
  snes.GetApu().LoadSpc(file);
  REQUIRE(snes.GetApu().PeekRam(0xFF03) == 0);
  REQUIRE(snes.GetApu().PeekRam(0xFF04) == 0xA5);
}
