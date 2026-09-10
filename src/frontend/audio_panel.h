#pragma once

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "frontend/audio_output.h"
#include "pupsnes/hw/apu/sdsp.h"

namespace pupsnes {
class SNES;
}

namespace pupsnes::frontend {

inline bool CanPlayAudio(bool running, float speed_multiplier) {
  return running && std::isfinite(speed_multiplier) && std::fabs(speed_multiplier - 1.0F) < 0.0001F;
}

// Keep parsing independent of ImGui and device initialization so malformed
// saved preferences can be checked in the headless test suite.
inline std::optional<float> ParseFiniteSetting(std::string_view value, float minimum, float maximum) {
  if (value.empty()) return std::nullopt;
  const std::string text(value);
  char* end = nullptr;
  errno = 0;
  const float parsed = std::strtof(text.c_str(), &end);
  if (end != text.c_str() + text.size() || errno == ERANGE || !std::isfinite(parsed)) return std::nullopt;
  return std::clamp(parsed, minimum, maximum);
}

inline std::optional<SdspMode> ParseSoundQuality(std::string_view value) {
  if (value == "0") return SdspMode::kSimple;
  if (value == "1") return SdspMode::kAccurate;
  return std::nullopt;
}

inline bool ParseAudioSetting(AudioSettings& settings, std::string_view key, std::string_view value) {
  if (key == "audio_enabled" || key == "audio_muted") {
    if (value == "0" || value == "1") {
      (key == "audio_enabled" ? settings.enabled : settings.muted) = value == "1";
    }
  } else if (key == "audio_volume") {
    if (const auto parsed = ParseFiniteSetting(value, 0.0F, 1.0F)) settings.volume = *parsed;
  } else if (key == "audio_latency_ms") {
    int parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec == std::errc{} && result.ptr == value.data() + value.size()) {
      settings.latency_ms = static_cast<uint32_t>(std::clamp(parsed, 20, 160));
    }
  } else if (key == "audio_device") {
    if (value.size() <= 4096 && value.find_first_of("\r\n") == std::string_view::npos &&
        value.find('\0') == std::string_view::npos) {
      settings.device_id = value;
    }
  } else {
    return false;
  }
  return true;
}

inline void WriteAudioSettings(std::ostream& stream, const AudioSettings& settings) {
  stream << "audio_enabled=" << (settings.enabled ? 1 : 0) << '\n';
  stream << "audio_muted=" << (settings.muted ? 1 : 0) << '\n';
  stream << "audio_volume=" << settings.volume << '\n';
  stream << "audio_latency_ms=" << settings.latency_ms << '\n';
  stream << "audio_device=" << settings.device_id << '\n';
}

struct AudioPanelState {
  bool show = false;
  bool devices_loaded = false;
  std::vector<AudioDevice> devices;
};

// Return true when persistent preferences (including pending sound quality)
// change. The owning frontend writes its existing config file.
bool RenderAudioMenu(AudioOutput& audio, AudioPanelState& panel);
bool RenderAudioPanel(AudioOutput& audio, SNES& snes, AudioPanelState& panel, bool running, float speed_multiplier);

}  // namespace pupsnes::frontend
