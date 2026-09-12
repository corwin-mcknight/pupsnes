#include "frontend/audio_buffer.h"

#include <algorithm>
#include <cmath>

namespace pupsnes::frontend {

static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

void AudioBuffer::Configure(uint32_t sample_rate, uint32_t latency_ms) noexcept {
  sample_rate_.store(sample_rate >= 8000 && sample_rate <= 384000 ? sample_rate : 48000, std::memory_order_relaxed);
  const uint32_t nominal_frames = std::clamp(latency_ms, 20U, 160U) * (kInputSampleRate / 1000);
  // Extra space lets recovery raise its prefill target and still accept two
  // video frames arriving together after a missed host refresh. This reserve
  // does not delay normal startup, and fits within the fixed ring storage.
  capacity_frames_.store(nominal_frames + 4 * kVideoBurstFrames, std::memory_order_relaxed);
  prefill_frames_.store(std::min(nominal_frames, nominal_frames / 2 + kVideoBurstFrames), std::memory_order_relaxed);
  Flush();
}

void AudioBuffer::SetActive(bool active) noexcept {
  if (active_.exchange(active, std::memory_order_acq_rel) != active) Flush();
}

void AudioBuffer::SetMuted(bool muted) noexcept { muted_.store(muted, std::memory_order_relaxed); }

void AudioBuffer::SetVolume(float volume) noexcept {
  volume_.store(std::isfinite(volume) ? std::clamp(volume, 0.0F, 1.0F) : 0.5F, std::memory_order_relaxed);
}

void AudioBuffer::Flush() noexcept {
  flush_cutoff_.store(write_index_.load(std::memory_order_relaxed), std::memory_order_release);
  generation_.fetch_add(1, std::memory_order_release);
}

bool AudioBuffer::PushSample(int16_t left, int16_t right) noexcept {
  if (!active_.load(std::memory_order_acquire)) return false;
  const uint64_t write = write_index_.load(std::memory_order_relaxed);
  const uint64_t read = read_index_.load(std::memory_order_acquire);
  if (write - read >= capacity_frames_.load(std::memory_order_relaxed)) {
    // Never overwrite unread memory, even during an in-flight callback.
    dropped_frames_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  frames_[write % kStorageFrames] = {left, right};
  write_index_.store(write + 1, std::memory_order_release);
  return true;
}

bool AudioBuffer::Pop(Frame& frame) noexcept {
  const uint64_t read = read_index_.load(std::memory_order_relaxed);
  if (read == write_index_.load(std::memory_order_acquire)) return false;
  frame = frames_[read % kStorageFrames];
  read_index_.store(read + 1, std::memory_order_release);
  return true;
}

void AudioBuffer::ResetInterpolation() noexcept {
  have_left_ = false;
  have_right_ = false;
  phase_ = 0;
  primed_ = false;
}

void AudioBuffer::ApplyFlush() noexcept {
  const uint64_t generation = generation_.load(std::memory_order_acquire);
  if (generation == consumer_generation_) return;
  // A callback may have consumed past an earlier flush while it was being
  // requested. Never move its cursor backwards and replay those samples.
  const uint64_t cutoff = flush_cutoff_.load(std::memory_order_acquire);
  const uint64_t read = read_index_.load(std::memory_order_relaxed);
  read_index_.store(std::max(read, cutoff), std::memory_order_release);
  consumer_generation_ = generation;
  ResetInterpolation();
  recovery_prefill_ = prefill_frames_.load(std::memory_order_relaxed);
  recovering_ = false;
  transition_remaining_ = 0;
  last_left_ = last_right_ = transition_left_ = transition_right_ = 0.0F;
}

void AudioBuffer::WriteOutput(std::span<float> output, float left, float right) noexcept {
  // Crossfade from the last output to silence or to the recovered stream.
  // Progress is in device frames, so callback slicing cannot change the ramp.
  if (transition_remaining_ > 0) {
    --transition_remaining_;
    const uint32_t transition_frames = sample_rate_.load(std::memory_order_relaxed) * kTransitionMs / 1000;
    const float remaining = static_cast<float>(transition_remaining_) / static_cast<float>(transition_frames);
    left = left * (1.0F - remaining) + transition_left_ * remaining;
    right = right * (1.0F - remaining) + transition_right_ * remaining;
  }
  last_left_ = left;
  last_right_ = right;
  const float gain = muted_.load(std::memory_order_relaxed) ? 0.0F : volume_.load(std::memory_order_relaxed);
  output[0] = left * gain;
  output[1] = right * gain;
}

void AudioBuffer::Render(std::span<float> output) noexcept {
  std::fill(output.begin(), output.end(), 0.0F);
  ApplyFlush();
  uint64_t underruns = 0;
  for (std::size_t index = 0; index + 1 < output.size(); index += 2) {
    ApplyFlush();
    if (!active_.load(std::memory_order_acquire)) continue;
    if (!primed_) {
      // Start with one device period plus a video frame (bounded by nominal
      // latency). After starvation, wait for the increased recovery target.
      const uint32_t prefill = recovery_prefill_;
      const uint64_t available =
          write_index_.load(std::memory_order_acquire) - read_index_.load(std::memory_order_relaxed);
      if (available < prefill) {
        ++underruns;
        WriteOutput(output.subspan(index, 2), 0.0F, 0.0F);
        continue;
      }
      primed_ = true;
      if (recovering_) {
        transition_left_ = last_left_;
        transition_right_ = last_right_;
        transition_remaining_ = sample_rate_.load(std::memory_order_relaxed) * kTransitionMs / 1000;
        recovering_ = false;
      }
    }
    const uint32_t rate = sample_rate_.load(std::memory_order_relaxed);
    if (!have_left_) have_left_ = Pop(left_);
    while (have_left_ && phase_ >= rate) {
      if (!have_right_) have_right_ = Pop(right_);
      if (!have_right_) break;
      phase_ -= rate;
      left_ = right_;
      have_right_ = false;
    }
    if (phase_ != 0 && !have_right_) have_right_ = Pop(right_);
    if (!have_left_ || phase_ >= rate || (phase_ != 0 && !have_right_)) {
      ++underruns;
      ResetInterpolation();
      // Grow only after an actual starvation, not for every silent frame.
      // Leave room for a 30 Hz producer burst above the recovery threshold.
      recovery_prefill_ = std::min(recovery_prefill_ + kVideoBurstFrames,
                                   capacity_frames_.load(std::memory_order_relaxed) - 2 * kVideoBurstFrames);
      recovering_ = true;
      transition_left_ = last_left_;
      transition_right_ = last_right_;
      transition_remaining_ = rate * kTransitionMs / 1000;
      WriteOutput(output.subspan(index, 2), 0.0F, 0.0F);
      continue;
    }
    const float fraction = static_cast<float>(phase_) / static_cast<float>(rate);
    const Frame right = have_right_ ? right_ : left_;
    const float sample_left = (static_cast<float>(left_.left) +
                               (static_cast<float>(right.left) - static_cast<float>(left_.left)) * fraction) /
                              32768.0F;
    const float sample_right = (static_cast<float>(left_.right) +
                                (static_cast<float>(right.right) - static_cast<float>(left_.right)) * fraction) /
                               32768.0F;
    WriteOutput(output.subspan(index, 2), sample_left, sample_right);
    phase_ += kInputSampleRate;
  }
  underrun_frames_.fetch_add(underruns, std::memory_order_relaxed);
}

AudioStats AudioBuffer::GetStats() const noexcept {
  const uint64_t read = read_index_.load(std::memory_order_acquire);
  const uint64_t write = write_index_.load(std::memory_order_acquire);
  const uint64_t cutoff = flush_cutoff_.load(std::memory_order_acquire);
  const uint64_t effective_read = std::max(read, cutoff);
  return {.queued_frames = write > effective_read ? write - effective_read : 0,
          .dropped_frames = dropped_frames_.load(std::memory_order_relaxed),
          .underrun_frames = underrun_frames_.load(std::memory_order_relaxed),
          .capacity_frames = capacity_frames_.load(std::memory_order_relaxed),
          .sample_rate = sample_rate_.load(std::memory_order_relaxed)};
}

}  // namespace pupsnes::frontend
