#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "pupsnes/hw/apu/spc700.h"

namespace pupsnes {

// Portable SPC snapshot, not an exact emulator save state. Internal DSP state
// and timer phases are absent from the format and are initialized on load.
struct SpcFile {
  Spc700::State cpu;
  std::array<uint8_t, 65536> ram{};
  std::array<uint8_t, 128> dsp{};
  std::string title;
  std::string game;
  std::string album;
  // Native 32 kHz stereo frames. No duration means play until stopped.
  std::optional<uint64_t> play_frames;
  uint64_t fade_frames = 0;

  [[nodiscard]] std::optional<uint64_t> EndFrame() const;
  [[nodiscard]] bool Finished(uint64_t frames) const;
  [[nodiscard]] int16_t FadeSample(int16_t sample, uint64_t frame) const;

  // Throws invalid_argument for an invalid signature or incomplete snapshot.
  // Reads ID666 and xid6 title/game and playback timing fields.
  [[nodiscard]] static SpcFile Parse(std::span<const uint8_t> bytes);
};

}  // namespace pupsnes
