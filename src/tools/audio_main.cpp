// Deterministic native-rate audio capture without a window or audio device.
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "pupsnes/core/snes.h"
#include "pupsnes/hw/apu/apu.h"
#include "pupsnes/hw/apu/spc_file.h"
#include "pupsnes/hw/rom/rom_format.h"
#include "pupsnes/tools/trace_runner.h"

namespace {
constexpr std::string_view kUsage =
    "Usage: pupsnes-audio (--rom PATH | --spc PATH) --seconds N --output PATH.wav\n"
    "                     [--skip-seconds N] [--dsp stub|third-party|native]\n"
    "                     [--quality gaussian|linear|simple] (third-party only)\n"
    "Capture 1-600 seconds of native 32 kHz, 16-bit stereo PCM.\n"
    "Skip up to 600 seconds of boot time. Existing output files are preserved.\n";

uint64_t ParseSeconds(std::string_view value) {
  uint64_t seconds = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
  if (error != std::errc{} || end != value.data() + value.size() || seconds > 600) {
    throw std::invalid_argument("seconds must be an integer from 0 to 600");
  }
  return seconds;
}

class WavOutput {
 public:
  WavOutput(const std::filesystem::path& path, uint32_t frames) : expected_frames_(frames) {
    if (std::filesystem::exists(path)) throw std::runtime_error("output already exists: " + path.string());
    stream_.exceptions(std::ios::failbit | std::ios::badbit);
    stream_.open(path, std::ios::binary | std::ios::noreplace);
    stream_.write("RIFF", 4);
    Write32(36 + frames * 4);
    stream_.write("WAVEfmt ", 8);
    Write32(16);
    Write16(1);  // Integer PCM.
    Write16(2);
    Write32(32000);
    Write32(128000);
    Write16(4);
    Write16(16);
    stream_.write("data", 4);
    Write32(frames * 4);
  }

  void Push(int16_t left, int16_t right) {
    if (frames_ >= expected_frames_) throw std::runtime_error("DSP generated too many samples");
    for (const int16_t sample : {left, right}) {
      const auto bits = static_cast<uint16_t>(sample);
      buffer_[used_++] = static_cast<char>(bits & 0xFFU);
      buffer_[used_++] = static_cast<char>(bits >> 8U);
    }
    ++frames_;
    if (used_ == buffer_.size()) Flush();
  }

  void Finish() {
    if (frames_ != expected_frames_) throw std::runtime_error("DSP generated too few samples");
    Flush();
    stream_.close();
  }

 private:
  void Write16(uint16_t value) {
    const std::array<char, 2> bytes = {static_cast<char>(value & 0xFFU), static_cast<char>(value >> 8U)};
    stream_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  void Write32(uint32_t value) {
    Write16(static_cast<uint16_t>(value & 0xFFFFU));
    Write16(static_cast<uint16_t>(value >> 16U));
  }
  void Flush() {
    stream_.write(buffer_.data(), static_cast<std::streamsize>(used_));
    used_ = 0;
  }

  std::ofstream stream_;
  std::array<char, 4096> buffer_{};
  std::size_t used_ = 0;
  uint32_t frames_ = 0;
  uint32_t expected_frames_;
};
}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path rom_path;
    std::filesystem::path spc_path;
    std::filesystem::path output_path;
    uint64_t seconds = 0;
    uint64_t skip_seconds = 0;
    auto mode = pupsnes::SdspMode::kAccurate;
    auto backend = pupsnes::SdspBackend::kThirdParty;
    for (int index = 1; index < argc; ++index) {
      const std::string_view arg = argv[index];
      if (arg == "--help" || arg == "-h") {
        std::cout << kUsage;
        return 0;
      }
      if (index + 1 >= argc) throw std::invalid_argument("missing value for " + std::string(arg));
      const std::string_view value = argv[++index];
      if (arg == "--rom") {
        rom_path = value;
      } else if (arg == "--spc") {
        spc_path = value;
      } else if (arg == "--output") {
        output_path = value;
      } else if (arg == "--seconds") {
        seconds = ParseSeconds(value);
      } else if (arg == "--skip-seconds") {
        skip_seconds = ParseSeconds(value);
      } else if (arg == "--quality" && value == "gaussian") {
        mode = pupsnes::SdspMode::kAccurate;
      } else if (arg == "--quality" && (value == "linear" || value == "simple")) {
        mode = pupsnes::SdspMode::kSimple;
      } else if (arg == "--dsp" && value == "stub") {
        backend = pupsnes::SdspBackend::kStub;
      } else if (arg == "--dsp" && value == "third-party") {
        backend = pupsnes::SdspBackend::kThirdParty;
      } else if (arg == "--dsp" && value == "native") {
        backend = pupsnes::SdspBackend::kNative;
      } else {
        throw std::invalid_argument("unknown option or value: " + std::string(arg));
      }
    }
    if (rom_path.empty() == spc_path.empty() || output_path.empty() || seconds == 0) {
      throw std::invalid_argument("exactly one of --rom/--spc, --output, and positive --seconds are required");
    }
    const auto& input_path = spc_path.empty() ? rom_path : spc_path;
    std::ifstream input(input_path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open input: " + input_path.string());
    std::vector<uint8_t> rom{std::istreambuf_iterator<char>(input), {}};
    pupsnes::SNES snes;
    snes.SetSdspModePending(mode);
    snes.SetSdspBackendPending(backend);
    if (spc_path.empty()) {
      pupsnes::StripSmcCopierHeader(rom);
      const auto loaded = snes.LoadRom(rom);
      if (!loaded.ok) throw std::runtime_error(loaded.message);
      snes.Reset();
    } else {
      snes.LoadSpc(pupsnes::SpcFile::Parse(rom));
    }
    WavOutput output(output_path, static_cast<uint32_t>(seconds * 32000));
    uint64_t skipped = 0;
    snes.SetAudioSampleCallback([&](int16_t left, int16_t right) {
      if (skipped < skip_seconds * 32000) {
        ++skipped;
      } else {
        output.Push(left, right);
      }
    });
    const auto spc_cycles = (seconds + skip_seconds) * 1024000;
    const auto target = (spc_cycles * pupsnes::Apu::kClockDenominator + pupsnes::Apu::kClockNumerator - 1) /
                        pupsnes::Apu::kClockNumerator;
    if (!spc_path.empty()) {
      snes.GetApu().CatchUpTo(target);
    } else {
      if (const auto error = pupsnes::tools::DriveMachineToMasterTime(snes, target)) {
        throw std::runtime_error(*error);
      }
    }
    snes.SetAudioSampleCallback({});
    output.Finish();
    std::cout << "Wrote " << seconds * 32000 << " stereo frames to " << output_path << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "pupsnes-audio: " << error.what() << '\n' << kUsage;
    return 1;
  }
}
