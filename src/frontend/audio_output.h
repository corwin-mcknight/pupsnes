#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "frontend/audio_buffer.h"

namespace pupsnes::frontend {

struct AudioSettings {
  bool enabled = true;
  bool muted = false;
  float volume = 0.5F;
  uint32_t latency_ms = 40;
  std::string device_id;
  bool operator==(const AudioSettings&) const = default;
};

struct AudioDevice {
  std::string id;
  std::string name;
  bool is_default = false;
};

[[nodiscard]] AudioSettings NormalizeAudioSettings(AudioSettings settings);

// Main/emulation-thread owner. The device callback only accesses its private
// AudioBuffer; it never reaches SNES or frontend state. Destruction/Shutdown
// stop and join callbacks before their storage is released. No capture device
// is opened. Device errors are nonfatal and can be retried explicitly.
class AudioOutput {
 public:
  AudioOutput();
  ~AudioOutput();
  AudioOutput(const AudioOutput&) = delete;
  AudioOutput& operator=(const AudioOutput&) = delete;
  AudioOutput(AudioOutput&&) = delete;
  AudioOutput& operator=(AudioOutput&&) = delete;

  [[nodiscard]] bool ApplySettings(const AudioSettings& settings);
  [[nodiscard]] bool Retry();
  void Shutdown();
  [[nodiscard]] std::vector<AudioDevice> EnumerateDevices();
  [[nodiscard]] const AudioSettings& GetSettings() const;
  [[nodiscard]] bool IsOpen() const;
  [[nodiscard]] const std::string& GetError() const;
  [[nodiscard]] uint32_t GetSampleRate() const;
  [[nodiscard]] AudioStats GetStats() const;

  // Frontends pass true only during normal 1x playback. Paused stepping and
  // other speeds discard samples; transitions flush queued and cached audio.
  void SetPlaybackActive(bool active);
  void Flush();
  void PushSample(int16_t left, int16_t right) noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pupsnes::frontend
