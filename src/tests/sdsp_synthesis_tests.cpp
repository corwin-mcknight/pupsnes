#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>

#include "pupsnes/hw/apu/accurate_sdsp.h"
#include "pupsnes/hw/apu/sdsp.h"
#include "pupsnes/hw/apu/simple_sdsp.h"

namespace {
using pupsnes::AccurateSdsp;
using pupsnes::Sdsp;
using pupsnes::SdspMode;
using pupsnes::SimpleSdsp;
using Aram = std::array<uint8_t, 65536>;
using Frame = std::array<int16_t, 2>;
constexpr std::array<uint8_t, 8> kBrrData = {0x12, 0x37, 0x6A, 0xCE, 0xFD, 0xB9, 0x80, 0x54};

void PutWord(Aram& aram, uint16_t address, uint16_t value) {
  aram[address] = static_cast<uint8_t>(value);
  aram[static_cast<uint16_t>(address + 1U)] = static_cast<uint8_t>(value >> 8);
}

void PutBrr(Aram& aram, uint16_t address, uint8_t header) {
  aram[address] = header;
  for (std::size_t i = 0; i < kBrrData.size(); ++i) {
    aram[static_cast<uint16_t>(address + i + 1U)] = kBrrData[i];
  }
}

void ConfigureVoice(Sdsp& dsp, unsigned voice, uint8_t source, uint16_t pitch = 0x0C80) {
  const auto base = static_cast<uint8_t>(voice * 16U);
  dsp.WriteRegister(base, 0x60);
  dsp.WriteRegister(static_cast<uint8_t>(base + 1), 0xB0);
  dsp.WriteRegister(static_cast<uint8_t>(base + 2), static_cast<uint8_t>(pitch));
  dsp.WriteRegister(static_cast<uint8_t>(base + 3), static_cast<uint8_t>(pitch >> 8));
  dsp.WriteRegister(static_cast<uint8_t>(base + 4), source);
  dsp.WriteRegister(static_cast<uint8_t>(base + 5), 0);
  dsp.WriteRegister(static_cast<uint8_t>(base + 7), 0x7F);
}

void Configure(Sdsp& dsp, Aram& aram, unsigned scenario) {
  PutWord(aram, 0x2000, 0x3000);
  PutWord(aram, 0x2002, 0x3000);
  PutBrr(aram, 0x3000, 0x93);
  dsp.WriteRegister(0x5D, 0x20);
  dsp.WriteRegister(0x0C, 0x70);
  dsp.WriteRegister(0x1C, 0x60);
  dsp.WriteRegister(0x6C, 0x20);
  ConfigureVoice(dsp, 0, 0);
  if (scenario < 4) aram[0x3000] = static_cast<uint8_t>(0x93U | (scenario << 2));
  if (scenario == 4) aram[0x3000] = 0xD3;
  if (scenario == 5) aram[0x3000] = 0xF3;
  if (scenario == 6) aram[0x3000] = 0x91;  // End without looping.
  if (scenario == 7) {
    dsp.WriteRegister(0x05, 0xFF);  // Fast attack and decay, timed sustain.
    dsp.WriteRegister(0x06, 0xA8);
  }
  if (scenario == 10) dsp.WriteRegister(0x07, 0xDF);  // Linear increasing GAIN.
  if (scenario == 11) dsp.WriteRegister(0x07, 0xFF);  // Bent increasing GAIN.
  if (scenario == 12) {
    dsp.WriteRegister(0x6C, 0x3F);
    dsp.WriteRegister(0x3D, 1);
  }
  if (scenario == 13) {
    PutWord(aram, 0x2004, 0x3040);
    PutWord(aram, 0x2006, 0x3040);
    PutBrr(aram, 0x3040, 0xA7);
    ConfigureVoice(dsp, 1, 1, 0x0900);
    dsp.WriteRegister(0x2D, 2);
  }
  if (scenario == 14) {
    aram[0x3000] = 0xC3;
    for (unsigned voice = 0; voice < 8; ++voice) ConfigureVoice(dsp, voice, 0, 0x1000);
    dsp.WriteRegister(0x0C, 0x7F);
    dsp.WriteRegister(0x1C, 0x80);
  }
  if (scenario == 15 || scenario == 16) {
    constexpr std::array<uint8_t, 8> kFir = {0x60, 0xD0, 0x20, 0xE0, 0x10, 0xF8, 0x04, 0xFC};
    for (std::size_t i = 0; i < kFir.size(); ++i) dsp.WriteRegister(static_cast<uint8_t>(i * 16 + 15), kFir[i]);
    dsp.WriteRegister(0x2C, 0x40);
    dsp.WriteRegister(0x3C, 0xD0);
    dsp.WriteRegister(0x0D, 0x50);
    dsp.WriteRegister(0x4D, 1);
    dsp.WriteRegister(0x6D, 0x60);
    dsp.WriteRegister(0x7D, scenario == 15 ? 1 : 0);
    dsp.WriteRegister(0x6C, 0);
  }
  dsp.WriteRegister(0x4C, scenario == 13 ? 3 : (scenario == 14 ? 0xFF : 1));
}

void ApplyEvent(Sdsp& dsp, unsigned scenario, unsigned frame) {
  if (scenario == 8 && frame == 64) dsp.WriteRegister(0x07, 0x9F);
  if (scenario == 9 && frame == 64) dsp.WriteRegister(0x07, 0xBF);
  if (scenario == 17 && frame == 128) dsp.WriteRegister(0x5C, 1);
  if (scenario == 17 && frame == 192) {
    dsp.WriteRegister(0x5C, 0);
    dsp.WriteRegister(0x4C, 1);
  }
}

uint64_t HashByte(uint64_t hash, uint8_t byte) { return (hash ^ byte) * 1099511628211ULL; }
uint64_t HashFrame(uint64_t hash, Frame frame) {
  for (const auto value : frame) {
    const auto bits = static_cast<uint16_t>(value);
    hash = HashByte(hash, static_cast<uint8_t>(bits));
    hash = HashByte(hash, static_cast<uint8_t>(bits >> 8));
  }
  return hash;
}

std::unique_ptr<Sdsp> MakeDsp(SdspMode mode, Aram& aram) {
  if (mode == SdspMode::kSimple) return std::make_unique<SimpleSdsp>(aram.data(), aram.size());
  return std::make_unique<AccurateSdsp>(aram.data(), aram.size());
}

Frame Sample(Sdsp& dsp) {
  Frame frame{};
  dsp.StepSample(frame[0], frame[1]);
  return frame;
}

void Clocks(Sdsp& dsp, unsigned count) {
  int16_t left = 0;
  int16_t right = 0;
  for (unsigned clock = 0; clock < count; ++clock) static_cast<void>(dsp.TickCycle(left, right));
}
}  // namespace

TEST_CASE("Accurate S-DSP matches independent snes_spc PCM ARAM and status goldens", "[unit][apu][sdsp][synthesis]") {
  // Generated with the UNMODIFIED accurate SPC_DSP.cpp/.h from snes_spc 0.9.0,
  // revision ec8ee2bbe30451614c1d02a83f7af1c97d497d45. The standalone driver
  // used these fixture writes, core.load(zero registers with FLG=E0), and
  // core.run(32) for each of 1024 frames. Fingerprints are FNV-1a over little
  // endian signed16 stereo PCM, all65536 ARAM bytes, and all128 DSP registers.
  // These values were generated independently of the PupSNES wrapper and its
  // interpolation/address-wrap adaptations. No external files are needed.
  struct Golden {
    const char* name;
    uint64_t pcm;
    uint64_t aram;
    uint64_t registers;
  };
  constexpr std::array<Golden, 18> kGoldens = {{
      {"BRR filter 0", 0x15504489A13543B7ULL, 0x8D6245A0325AA6F9ULL, 0xEDCCA25DCF368715ULL},
      {"BRR filter 1", 0x4549B4D10D58D8FFULL, 0xB75375ED89E03325ULL, 0x008A124A2892774BULL},
      {"BRR filter 2", 0x2C7E7D68AC2297EEULL, 0xA92918619957FFE1ULL, 0xC2C3D866772FA26CULL},
      {"BRR filter 3", 0xAD89D54DDF2AB912ULL, 0xBD49ACE38CCB126DULL, 0xBBC67C16D860092CULL},
      {"BRR invalid range 13", 0x0BD8F7007F2E2E59ULL, 0xB1A8DB03EF809CB9ULL, 0x1F4B2BD973439FB3ULL},
      {"BRR invalid range 15", 0x0BD8F7007F2E2E59ULL, 0x3BA92B1953807699ULL, 0x1F4B2BD973439FB3ULL},
      {"BRR end without loop", 0xB93A0C83CE3B6325ULL, 0xEED0B538590497F7ULL, 0x7E7A4386584CF47CULL},
      {"ADSR attack decay sustain", 0x0012B50820C5CAE8ULL, 0x8D6245A0325AA6F9ULL, 0xE51620A9D28C8FB6ULL},
      {"GAIN linear decrease", 0xFAA10CB888D8252FULL, 0x8D6245A0325AA6F9ULL, 0x3A28CC1B227B725CULL},
      {"GAIN exponential decrease", 0x4C8508949DBC7E66ULL, 0x8D6245A0325AA6F9ULL, 0xF30C2B031A3F563CULL},
      {"GAIN linear increase", 0x991DD2F509DDA6D5ULL, 0x8D6245A0325AA6F9ULL, 0xF85E2A68AE8D53B5ULL},
      {"GAIN bent increase", 0x3C63A7EC522B17C1ULL, 0x8D6245A0325AA6F9ULL, 0x21D7C288523CC295ULL},
      {"Noise", 0x323048D4580335B3ULL, 0x8D6245A0325AA6F9ULL, 0xA3E52FC221FDFC27ULL},
      {"Pitch modulation", 0x7D3B268D6B217968ULL, 0x2A49E8327E64DE01ULL, 0x376BE59D54A30E2DULL},
      {"Eight voice signed mix and clipping", 0x6536F1E40D292ECDULL, 0x8B9A32E5F1327809ULL, 0x1CB00DA89120308AULL},
      {"Echo FIR feedback and buffer wrap", 0xF74CA44AEBC96A2BULL, 0x251E504721868712ULL, 0x3717D1D18966A541ULL},
      {"Echo zero delay", 0xBB38099C6A1AA9DEULL, 0xA0F307DBB6655DE1ULL, 0x2E6EA6D1847F2E86ULL},
      {"KOFF release and KON retrigger", 0x3868E8A9386C54BCULL, 0x8D6245A0325AA6F9ULL, 0xEB4F265F976CAB2BULL},
  }};
  for (unsigned scenario = 0; scenario < kGoldens.size(); ++scenario) {
    const auto& golden = kGoldens[scenario];
    INFO(golden.name);
    Aram aram{};
    AccurateSdsp dsp(aram.data(), aram.size());
    Configure(dsp, aram, scenario);
    uint64_t pcm_hash = 14695981039346656037ULL;
    for (unsigned frame = 0; frame < 1024; ++frame) {
      ApplyEvent(dsp, scenario, frame);
      pcm_hash = HashFrame(pcm_hash, Sample(dsp));
    }
    uint64_t aram_hash = 14695981039346656037ULL;
    for (const auto value : aram) aram_hash = HashByte(aram_hash, value);
    uint64_t register_hash = 14695981039346656037ULL;
    for (uint8_t index = 0; index < 128; ++index) register_hash = HashByte(register_hash, dsp.ReadRegister(index));
    REQUIRE(pcm_hash == golden.pcm);
    REQUIRE(aram_hash == golden.aram);
    REQUIRE(register_hash == golden.registers);
  }
}

TEST_CASE("S-DSP Gaussian startup produces literal signed stereo reference samples", "[unit][apu][sdsp][synthesis]") {
  // Same independent upstream reference as the fingerprints, retained as
  // explicit samples to expose key-on latency and channel/sign regressions.
  constexpr std::array<Frame, 32> kExpected = {{
      {0, 0},        {0, 0},        {0, 0},       {0, 0},        {0, 0},        {0, 0},       {0, 0},
      {0, 0},        {666, -477},   {1019, -729}, {1706, -1219}, {2081, -1488}, {1043, -746}, {-992, 707},
      {-1501, 1071}, {-1029, 735},  {-619, 441},  {-522, 372},   {-882, 630},   {-1401, 999}, {-1916, 1367},
      {-2345, 1674}, {-2187, 1561}, {-798, 568},  {751, -538},   {1420, -1015}, {1166, -834}, {637, -456},
      {559, -400},   {817, -585},   {1346, -963}, {2013, -1439},
  }};
  Aram aram{};
  AccurateSdsp dsp(aram.data(), aram.size());
  Configure(dsp, aram, 0);
  for (std::size_t index = 0; index < kExpected.size(); ++index) {
    CAPTURE(index);
    REQUIRE(Sample(dsp) == kExpected[index]);
  }
}

TEST_CASE("S-DSP status writes are replaced on their individual voice clocks", "[unit][apu][sdsp][synthesis]") {
  for (const auto mode : {SdspMode::kSimple, SdspMode::kAccurate}) {
    CAPTURE(mode);
    Aram aram{};
    auto dsp = MakeDsp(mode, aram);
    dsp->WriteRegister(0x08, 0xAA);
    dsp->WriteRegister(0x09, 0xBB);
    Clocks(*dsp, 3);
    REQUIRE(dsp->ReadRegister(0x08) == 0xAA);
    REQUIRE(dsp->ReadRegister(0x09) == 0xBB);
    Clocks(*dsp, 1);  // Voice0 V8 at phase3 writes OUTX.
    REQUIRE(dsp->ReadRegister(0x09) == 0);
    REQUIRE(dsp->ReadRegister(0x08) == 0xAA);
    Clocks(*dsp, 1);  // Voice0 V9 at phase4 writes ENVX.
    REQUIRE(dsp->ReadRegister(0x08) == 0);

    dsp->Reset();
    Clocks(*dsp, 2);                 // V6 has calculated the next OUTX byte.
    dsp->WriteRegister(0x09, 0x77);  // CPU write also replaces the pipeline buffer.
    Clocks(*dsp, 2);
    REQUIRE(dsp->ReadRegister(0x09) == 0x77);
    Clocks(*dsp, 32);
    REQUIRE(dsp->ReadRegister(0x09) == 0);

    dsp->Reset();
    Clocks(*dsp, 3);  // V7 has calculated the next ENVX byte.
    dsp->WriteRegister(0x08, 0x55);
    Clocks(*dsp, 2);
    REQUIRE(dsp->ReadRegister(0x08) == 0x55);
    Clocks(*dsp, 32);
    REQUIRE(dsp->ReadRegister(0x08) == 0);
  }
}

TEST_CASE("S-DSP echo reads and signed FIR samples use exact per-channel clocks", "[unit][apu][sdsp][synthesis]") {
  for (const auto mode : {SdspMode::kSimple, SdspMode::kAccurate}) {
    CAPTURE(mode);
    Aram aram{};
    auto dsp = MakeDsp(mode, aram);
    PutWord(aram, 0, 16384);
    PutWord(aram, 2, static_cast<uint16_t>(-16384));
    dsp->WriteRegister(0x6C, 0x20);  // Echo write disabled; reads and output remain active.
    dsp->WriteRegister(0x7F, 64);    // FIR7 is the newest sample, coefficient1 in Q6.
    dsp->WriteRegister(0x2C, 64);
    dsp->WriteRegister(0x3C, 64);
    Clocks(*dsp, 23);        // Phase22 reads left echo.
    PutWord(aram, 0, 0);     // Too late to change the left sample in this frame.
    PutWord(aram, 2, 8192);  // Phase23 has not read right echo yet.
    Frame output = {-1, -1};
    for (unsigned cycle = 24; cycle <= 31; ++cycle) REQUIRE_FALSE(dsp->TickCycle(output[0], output[1]));
    REQUIRE(output == Frame{-1, -1});
    REQUIRE(dsp->TickCycle(output[0], output[1]));
    // Echo history halves16-bit samples; coefficient64 retains that value,
    // then EVOL64 halves it again: 16384/4 and8192/4 respectively.
    REQUIRE(output == Frame{4096, 2048});
    REQUIRE(aram[2] == 0);
    REQUIRE(aram[3] == 0x20);  // Echo write-disable preserves the backing sample.
  }
}

TEST_CASE("S-DSP echo writes and FLG latches occur before sample delivery", "[unit][apu][sdsp][synthesis]") {
  for (const bool disable_between_channels : {false, true}) {
    CAPTURE(disable_between_channels);
    Aram aram{};
    AccurateSdsp dsp(aram.data(), aram.size());
    PutWord(aram, 0, 0x1234);
    PutWord(aram, 2, 0x5678);
    dsp.WriteRegister(0x6C, 0);
    Clocks(dsp, 29);  // Phase28 latches enabled echo writes.
    REQUIRE(aram[0] == 0x34);
    REQUIRE(aram[2] == 0x78);
    if (disable_between_channels) dsp.WriteRegister(0x6C, 0x20);
    Clocks(dsp, 1);  // Phase29 writes left and re-latches FLG for right.
    REQUIRE(aram[0] == 0);
    REQUIRE(aram[1] == 0);
    REQUIRE(aram[2] == 0x78);
    Clocks(dsp, 1);  // Phase30 writes right unless the new FLG disables it.
    REQUIRE(aram[2] == (disable_between_channels ? 0x78 : 0));
    REQUIRE(aram[3] == (disable_between_channels ? 0x56 : 0));
  }
}

TEST_CASE("Both S-DSP modes key on loop release and retrigger real voices", "[unit][apu][sdsp][synthesis]") {
  for (const auto mode : {SdspMode::kSimple, SdspMode::kAccurate}) {
    CAPTURE(mode);
    Aram aram{};
    auto dsp = MakeDsp(mode, aram);
    Configure(*dsp, aram, 0);
    bool heard = false;
    for (unsigned frame = 0; frame < 64; ++frame) heard |= Sample(*dsp) != Frame{0, 0};
    REQUIRE(heard);
    REQUIRE(dsp->ReadRegister(0x08) == 0x7F);
    REQUIRE(dsp->ReadRegister(0x7C) == 1);
    dsp->WriteRegister(0x7C, 0xFF);
    REQUIRE(dsp->ReadRegister(0x7C) == 0);
    for (unsigned frame = 0; frame < 32; ++frame) static_cast<void>(Sample(*dsp));
    REQUIRE(dsp->ReadRegister(0x7C) == 1);

    dsp->WriteRegister(0x5C, 1);
    for (unsigned frame = 0; frame < 260; ++frame) static_cast<void>(Sample(*dsp));
    REQUIRE(dsp->ReadRegister(0x08) == 0);
    REQUIRE(Sample(*dsp) == Frame{0, 0});
    dsp->WriteRegister(0x5C, 0);
    dsp->WriteRegister(0x4C, 1);
    bool cleared_end = false;
    for (unsigned frame = 0; frame < 6; ++frame) {
      static_cast<void>(Sample(*dsp));
      cleared_end |= dsp->ReadRegister(0x7C) == 0;
    }
    REQUIRE(cleared_end);
    heard = false;
    for (unsigned frame = 0; frame < 32; ++frame) heard |= Sample(*dsp) != Frame{0, 0};
    REQUIRE(heard);
    REQUIRE(dsp->ReadRegister(0x08) == 0x7F);
  }
}

TEST_CASE("S-DSP directory and BRR fetches wrap within shared 64 KiB ARAM", "[unit][apu][sdsp][synthesis]") {
  for (const auto mode : {SdspMode::kSimple, SdspMode::kAccurate}) {
    CAPTURE(mode);
    Aram ordinary_aram{};
    Aram wrapped_aram{};
    auto ordinary = MakeDsp(mode, ordinary_aram);
    auto wrapped = MakeDsp(mode, wrapped_aram);
    Configure(*ordinary, ordinary_aram, 0);
    Configure(*wrapped, wrapped_aram, 0);
    // DIR=$FF plus SRCN=$40 gives directory entry$10000, wrapping to$0000.
    // Its BRR block crosses$FFFF->$0000, so use a different wrapped directory
    // entry at$0008 (SRCN=$42) to keep both test fixtures independently intact.
    wrapped->WriteRegister(0x5D, 0xFF);
    wrapped->WriteRegister(0x04, 0x42);
    PutWord(wrapped_aram, 8, 0xFFFC);
    PutWord(wrapped_aram, 10, 0xFFFC);
    PutBrr(wrapped_aram, 0xFFFC, 0x93);
    for (unsigned frame = 0; frame < 256; ++frame) REQUIRE(Sample(*ordinary) == Sample(*wrapped));
    REQUIRE(ordinary->ReadRegister(0x08) == wrapped->ReadRegister(0x08));
    REQUIRE(ordinary->ReadRegister(0x09) == wrapped->ReadRegister(0x09));
    REQUIRE(ordinary->ReadRegister(0x7C) == wrapped->ReadRegister(0x7C));
  }
}

TEST_CASE("S-DSP interpolation selection is per instance and resets preserve its sound",
          "[unit][apu][sdsp][synthesis]") {
  Aram simple_aram{};
  Aram accurate_aram{};
  SimpleSdsp simple(simple_aram.data(), simple_aram.size());
  AccurateSdsp accurate(accurate_aram.data(), accurate_aram.size());
  Configure(simple, simple_aram, 0);
  Configure(accurate, accurate_aram, 0);
  std::array<Frame, 128> simple_pcm{};
  std::array<Frame, 128> accurate_pcm{};
  for (std::size_t frame = 0; frame < simple_pcm.size(); ++frame) {
    simple_pcm[frame] = Sample(simple);
    accurate_pcm[frame] = Sample(accurate);
  }
  REQUIRE(simple_pcm != accurate_pcm);
  REQUIRE(std::ranges::any_of(simple_pcm, [](Frame frame) { return frame != Frame{0, 0}; }));
  REQUIRE(std::ranges::any_of(accurate_pcm, [](Frame frame) { return frame != Frame{0, 0}; }));
  Clocks(simple, 13);
  Clocks(accurate, 29);
  simple.Reset();
  accurate.Reset();
  Configure(simple, simple_aram, 0);
  Configure(accurate, accurate_aram, 0);
  for (std::size_t frame = 0; frame < simple_pcm.size(); ++frame) {
    REQUIRE(Sample(simple) == simple_pcm[frame]);
    REQUIRE(Sample(accurate) == accurate_pcm[frame]);
  }
}

TEST_CASE("S-DSP sample stepping and single clocks agree across partial phases", "[unit][apu][sdsp][synthesis]") {
  for (const auto mode : {SdspMode::kSimple, SdspMode::kAccurate}) {
    CAPTURE(mode);
    Aram sample_aram{};
    Aram clock_aram{};
    auto sample_dsp = MakeDsp(mode, sample_aram);
    auto clock_dsp = MakeDsp(mode, clock_aram);
    Configure(*sample_dsp, sample_aram, 15);
    Configure(*clock_dsp, clock_aram, 15);
    Clocks(*sample_dsp, 13);
    Clocks(*clock_dsp, 13);
    for (unsigned frame = 0; frame < 128; ++frame) {
      sample_dsp->WriteRegister(0x02, static_cast<uint8_t>(frame * 37));
      clock_dsp->WriteRegister(0x02, static_cast<uint8_t>(frame * 37));
      const auto expected = Sample(*sample_dsp);
      Frame actual = {123, -456};
      unsigned deliveries = 0;
      for (unsigned cycle = 0; cycle < 32; ++cycle) deliveries += clock_dsp->TickCycle(actual[0], actual[1]) ? 1U : 0U;
      REQUIRE(deliveries == 1);
      REQUIRE(actual == expected);
      REQUIRE(sample_aram == clock_aram);
      for (uint8_t index = 0; index < 128; ++index) {
        REQUIRE(sample_dsp->ReadRegister(index) == clock_dsp->ReadRegister(index));
      }
    }
  }
}

TEST_CASE("S-DSP rejects missing or incorrectly sized shared ARAM", "[unit][apu][sdsp][synthesis]") {
  Aram aram{};
  REQUIRE_THROWS_AS(AccurateSdsp(nullptr, aram.size()), std::invalid_argument);
  REQUIRE_THROWS_AS(SimpleSdsp(aram.data(), aram.size() - 1), std::invalid_argument);
}
