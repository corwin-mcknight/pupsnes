#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pupsnes::frontend {

struct AudioStats {
  uint64_t queued_frames = 0;
  uint64_t dropped_frames = 0;
  uint64_t underrun_frames = 0;
  uint32_t capacity_frames = 0;
  uint32_t sample_rate = 0;
};

// One emulator/UI producer and one host callback consumer. No method allocates
// or waits on a lock. Flush publishes a cutoff; only the consumer advances its
// read cursor, so a reset cannot race with an in-flight ring-buffer read.
// Cache-line separation keeps producer/consumer cursor traffic independent.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
class AudioBuffer {
 public:
  static constexpr uint32_t kInputSampleRate = 32000;
  static constexpr std::size_t kStorageFrames = 8192;

  AudioBuffer() = default;
  AudioBuffer(const AudioBuffer&) = delete;
  AudioBuffer& operator=(const AudioBuffer&) = delete;

  // Producer controls. Configure is called with the device callback stopped.
  void Configure(uint32_t sample_rate, uint32_t latency_ms) noexcept;
  void SetActive(bool active) noexcept;
  void SetMuted(bool muted) noexcept;
  void SetVolume(float volume) noexcept;
  void Flush() noexcept;
  [[nodiscard]] bool PushSample(int16_t left, int16_t right) noexcept;
  [[nodiscard]] AudioStats GetStats() const noexcept;

  // Consumer only. Fills interleaved stereo float output; any underflow or
  // inactive playback is silence. Odd trailing samples are also zeroed.
  void Render(std::span<float> output) noexcept;

 private:
  struct Frame {
    int16_t left = 0;
    int16_t right = 0;
  };

  static constexpr uint32_t kVideoBurstFrames = 544;  // 17 ms at 32 kHz.

  [[nodiscard]] bool Pop(Frame& frame) noexcept;
  void ApplyFlush() noexcept;
  void ResetInterpolation() noexcept;

  std::array<Frame, kStorageFrames> frames_{};
  alignas(64) std::atomic<uint64_t> write_index_{0};
  alignas(64) std::atomic<uint64_t> read_index_{0};
  std::atomic<uint64_t> flush_cutoff_{0};
  std::atomic<uint64_t> generation_{0};
  std::atomic<uint64_t> dropped_frames_{0};
  std::atomic<uint64_t> underrun_frames_{0};
  std::atomic<uint32_t> capacity_frames_{1280 + 2 * kVideoBurstFrames};
  std::atomic<uint32_t> prefill_frames_{1184};
  std::atomic<uint32_t> sample_rate_{48000};
  std::atomic<float> volume_{0.5F};
  std::atomic<bool> active_{false};
  std::atomic<bool> muted_{false};

  // Accessed only by Render on the consumer thread.
  uint64_t consumer_generation_ = 0;
  uint32_t phase_ = 0;
  Frame left_{};
  Frame right_{};
  bool have_left_ = false;
  bool have_right_ = false;
  bool primed_ = false;
};

}  // namespace pupsnes::frontend
