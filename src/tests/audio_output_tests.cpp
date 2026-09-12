#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

#include "frontend/audio_buffer.h"
#include "frontend/audio_output.h"

namespace {

using pupsnes::frontend::AudioBuffer;
using pupsnes::frontend::AudioOutput;
using pupsnes::frontend::AudioSettings;
using pupsnes::frontend::NormalizeAudioSettings;

bool Fill(AudioBuffer& buffer, std::size_t count, int16_t left = 16384, int16_t right = -16384) {
  bool accepted = true;
  for (std::size_t index = 0; index < count; ++index) accepted &= buffer.PushSample(left, right);
  return accepted;
}

bool Silent(std::span<const float> samples) {
  return std::all_of(samples.begin(), samples.end(), [](float value) { return value == 0.0F; });
}

void Activate(AudioBuffer& buffer, uint32_t rate = 32000, uint32_t latency = 20) {
  buffer.Configure(rate, latency);
  buffer.SetVolume(1.0F);
  buffer.SetActive(true);
}

}  // namespace

TEST_CASE("Host audio settings normalize finite gain and bounded latency without changing device selection",
          "[unit][audio_output]") {
  const AudioSettings defaults;
  REQUIRE(defaults.enabled);
  REQUIRE_FALSE(defaults.muted);
  REQUIRE(defaults.volume == 0.5F);
  REQUIRE(defaults.latency_ms == 40);
  REQUIRE(defaults.device_id.empty());
  for (const float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity()}) {
    REQUIRE(NormalizeAudioSettings({.volume = invalid, .device_id = {}}).volume == 0.5F);
  }
  REQUIRE(NormalizeAudioSettings({.volume = -1.0F, .device_id = {}}).volume == 0.0F);
  REQUIRE(NormalizeAudioSettings({.volume = 2.0F, .device_id = {}}).volume == 1.0F);
  REQUIRE(NormalizeAudioSettings({.latency_ms = 0, .device_id = {}}).latency_ms == 20);
  REQUIRE(NormalizeAudioSettings({.latency_ms = UINT32_MAX, .device_id = {}}).latency_ms == 160);
  const AudioSettings custom{false, true, 0.75F, 80, "Core Audio:313233"};
  REQUIRE(NormalizeAudioSettings(custom) == custom);
}

TEST_CASE("Host audio starts inactive and silence never accumulates paused samples", "[unit][audio_output]") {
  AudioBuffer buffer;
  std::array<float, 11> output;
  output.fill(1.0F);
  REQUIRE_FALSE(Fill(buffer, 800));
  buffer.Render(output);
  REQUIRE(Silent(output));
  REQUIRE(buffer.GetStats().queued_frames == 0);
  REQUIRE(buffer.GetStats().dropped_frames == 0);
  REQUIRE(buffer.GetStats().underrun_frames == 0);
}

TEST_CASE("Host audio primes with a video frame of samples and refills after an underrun", "[unit][audio_output]") {
  AudioBuffer buffer;
  Activate(buffer, 32000, 20);
  REQUIRE(buffer.GetStats().capacity_frames == 2816);
  REQUIRE(Fill(buffer, 639));
  std::array<float, 8> output;
  buffer.Render(output);
  REQUIRE(Silent(output));
  REQUIRE(buffer.GetStats().queued_frames == 639);
  REQUIRE(buffer.GetStats().underrun_frames == 4);
  REQUIRE(buffer.PushSample(16384, -16384));
  std::vector<float> all(640 * 2);
  buffer.Render(all);
  bool correct = true;
  for (std::size_t index = 0; index < all.size(); index += 2) {
    correct &= all[index] == 0.5F && all[index + 1] == -0.5F;
  }
  REQUIRE(correct);
  REQUIRE(buffer.GetStats().queued_frames == 0);
  buffer.Render(output);
  REQUIRE(output[0] > 0.0F);
  REQUIRE(output[0] < 0.5F);  // Starvation fades the last sample instead of cutting it off.
  REQUIRE(buffer.GetStats().underrun_frames == 8);
  std::array<float, 192> tail;
  buffer.Render(tail);
  REQUIRE(tail.back() == 0.0F);
  REQUIRE(Fill(buffer, 1183, -8192, 8192));
  buffer.Render(output);  // Recovery waits for one extra video frame.
  REQUIRE(Silent(output));
  REQUIRE(buffer.PushSample(-8192, 8192));
  buffer.Render(tail);
  REQUIRE(tail[0] < 0.0F);
  REQUIRE(tail[0] > -0.01F);
  REQUIRE(tail[190] == -0.25F);
  REQUIRE(tail[191] == 0.25F);
}

TEST_CASE("Host audio linearly resamples 32 kHz stereo to 48 kHz with independent channels", "[unit][audio_output]") {
  AudioBuffer buffer;
  Activate(buffer, 48000);
  for (int16_t value : std::array<int16_t, 7>{0, 12000, 24000, 12000, 0, -12000, -24000}) {
    REQUIRE(buffer.PushSample(value, static_cast<int16_t>(-value)));
  }
  REQUIRE(Fill(buffer, 633, 0, 0));
  std::array<float, 20> output;
  buffer.Render(output);
  constexpr std::array<float, 10> kExpected{0, 8000, 16000, 24000, 16000, 8000, 0, -8000, -16000, -24000};
  for (std::size_t frame = 0; frame < kExpected.size(); ++frame) {
    CAPTURE(frame);
    REQUIRE(output[2 * frame] == Catch::Approx(kExpected[frame] / 32768.0F).margin(0.000001));
    REQUIRE(output[2 * frame + 1] == Catch::Approx(-kExpected[frame] / 32768.0F).margin(0.000001));
  }
}

TEST_CASE("Host audio priming covers a complete native callback with interpolation lookahead", "[unit][audio_output]") {
  struct Preset {
    uint32_t latency;
    uint32_t prime;
    uint32_t capacity;
  };
  for (const auto [latency, prime, capacity] :
       std::array<Preset, 4>{{{20, 640, 2816}, {40, 1184, 3456}, {80, 1824, 4736}, {160, 3104, 7296}}}) {
    CAPTURE(latency, prime, capacity);
    AudioBuffer buffer;
    Activate(buffer, 48000, latency);
    // Physical burst headroom must not postpone the established start target.
    REQUIRE(buffer.GetStats().capacity_frames == capacity);
    REQUIRE(Fill(buffer, prime - 1));
    std::array<float, 2> waiting;
    buffer.Render(waiting);
    REQUIRE(Silent(waiting));
    REQUIRE(buffer.GetStats().queued_frames == prime - 1);
    REQUIRE(buffer.PushSample(16384, -16384));
    std::vector<float> callback(latency * 24 * 2);
    buffer.Render(callback);
    bool correct = true;
    for (std::size_t index = 0; index < callback.size(); index += 2) {
      correct &= callback[index] == 0.5F && callback[index + 1] == -0.5F;
    }
    REQUIRE(correct);
    REQUIRE(buffer.GetStats().underrun_frames == 1);
    REQUIRE(buffer.GetStats().queued_frames >= 319);
  }
}

TEST_CASE("Host audio downsampling advances by the input to output clock ratio", "[unit][audio_output]") {
  AudioBuffer buffer;
  Activate(buffer, 16000);
  for (int16_t value = 0; value < 10000; value += 1000) REQUIRE(buffer.PushSample(value, 5000));
  REQUIRE(Fill(buffer, 630, 0, 5000));
  std::array<float, 10> output;
  buffer.Render(output);
  for (std::size_t frame = 0; frame < 5; ++frame) {
    CAPTURE(frame);
    REQUIRE(output[frame * 2] == static_cast<float>(frame * 2000) / 32768.0F);
    REQUIRE(output[frame * 2 + 1] == 5000.0F / 32768.0F);
  }
}

TEST_CASE("Host audio accepts GUI frame bursts across callback phases without startup loss", "[unit][audio_output]") {
  for (uint32_t latency : {20U, 40U, 80U, 160U}) {
    const uint64_t callback_period = static_cast<uint64_t>(latency) * 500000;
    for (uint64_t phase = 0; phase < callback_period; phase += 1000000) {
      for (bool producer_first : {false, true}) {
        CAPTURE(latency, phase, producer_first);
        AudioBuffer buffer;
        Activate(buffer, 48000, latency);
        std::vector<float> callback(latency * 24 * 2);
        bool accepted = true;
        bool heard_audio = false;
        bool continuous = true;
        bool bounded = true;
        uint64_t produced = 0;
        uint64_t frame = 0;
        uint64_t next_callback = phase;
        while (frame < 60 || next_callback < 1000000000) {
          const uint64_t next_frame = frame < 60 ? frame * 1000000000 / 60 : UINT64_MAX;
          if (next_frame < next_callback || (producer_first && next_frame == next_callback)) {
            const uint64_t target = (frame + 1) * 32000 / 60;
            while (produced < target) {
              accepted &= buffer.PushSample(8192, -8192);
              ++produced;
            }
            ++frame;
          } else {
            buffer.Render(callback);
            for (std::size_t index = 0; index < callback.size(); index += 2) {
              if (callback[index] != 0.0F) heard_audio = true;
              if (heard_audio) continuous &= callback[index] == 0.25F && callback[index + 1] == -0.25F;
            }
            next_callback += callback_period;
          }
          bounded &= buffer.GetStats().queued_frames <= buffer.GetStats().capacity_frames;
        }
        REQUIRE(accepted);
        REQUIRE(heard_audio);
        REQUIRE(continuous);
        REQUIRE(bounded);
        REQUIRE(buffer.GetStats().dropped_frames == 0);
      }
    }
  }
}

TEST_CASE("Host audio resampling is independent of native callback slicing", "[unit][audio_output]") {
  for (uint32_t rate : {8000U, 16000U, 32000U, 44100U, 48000U, 96000U, 192000U, 384000U}) {
    CAPTURE(rate);
    AudioBuffer whole;
    AudioBuffer sliced;
    Activate(whole, rate, 40);
    Activate(sliced, rate, 40);
    bool accepted = true;
    for (int index = 0; index < 1280; ++index) {
      const auto left = static_cast<int16_t>((index * 71) % 60001 - 30000);
      const auto right = static_cast<int16_t>((index * 137) % 60001 - 30000);
      accepted &= whole.PushSample(left, right) && sliced.PushSample(left, right);
    }
    REQUIRE(accepted);
    std::vector<float> expected(rate / 50 * 2);
    std::vector<float> actual(expected.size());
    whole.Render(expected);
    std::size_t offset = 0;
    for (std::size_t chunk = 1; offset < actual.size(); chunk = chunk % 53 + 1) {
      const std::size_t count = std::min(chunk * 2, actual.size() - offset);
      sliced.Render(std::span(actual).subspan(offset, count));
      offset += count;
    }
    REQUIRE(actual == expected);
    REQUIRE(sliced.GetStats().queued_frames == whole.GetStats().queued_frames);
    REQUIRE(sliced.GetStats().underrun_frames == 0);
  }
}

TEST_CASE("Host audio recovers from slower GUI delivery without repeated gaps", "[unit][audio_output]") {
  for (uint32_t latency : {20U, 40U, 80U, 160U}) {
    for (uint32_t rate : {32000U, 44100U, 48000U}) {
      CAPTURE(latency, rate);
      const uint64_t callback_period = static_cast<uint64_t>(latency) * 500000;
      std::vector<float> callback(rate * latency / 2000 * 2);
      for (const uint64_t phase : {uint64_t{0}, callback_period / 3, callback_period - 1}) {
        CAPTURE(phase);
        AudioBuffer buffer;
        Activate(buffer, rate, latency);
        uint64_t next_callback = phase;
        uint64_t produced = 0;
        uint64_t settled_underruns = 0;
        bool accepted = true;
        // 60 Hz, then two seconds at 30 Hz, then back to 60 Hz. Emulated time still
        // tracks wall time, as in the frontends, but arrives in larger bursts.
        for (uint64_t frame = 0; frame < 180; ++frame) {
          const uint64_t time = frame < 60    ? frame * 1000000000 / 60
                                : frame < 120 ? 1000000000 + (frame - 60) * 1000000000 / 30
                                              : 3000000000 + (frame - 120) * 1000000000 / 60;
          while (next_callback <= time) {
            buffer.Render(callback);
            next_callback += callback_period;
          }
          const uint64_t target = time * 32000 / 1000000000;
          while (produced < target) {
            accepted &= buffer.PushSample(8192, -8192);
            ++produced;
          }
          if (frame == 90) settled_underruns = buffer.GetStats().underrun_frames;
        }
        REQUIRE(accepted);
        REQUIRE(buffer.GetStats().dropped_frames == 0);
        REQUIRE(buffer.GetStats().underrun_frames == settled_underruns);
        REQUIRE_FALSE(Silent(callback));
      }
    }
  }
}

TEST_CASE("Host audio fades across starvation and recovery instead of clicking", "[unit][audio_output]") {
  AudioBuffer buffer;
  Activate(buffer);
  REQUIRE(Fill(buffer, 640));
  std::vector<float> signal(640 * 2);
  buffer.Render(signal);
  REQUIRE(signal.back() == -0.5F);
  std::vector<float> gap(320 * 2);
  buffer.Render(gap);
  float previous = 0.5F;
  for (std::size_t index = 0; index < gap.size(); index += 2) {
    REQUIRE(std::abs(gap[index] - previous) <= 0.006F);
    REQUIRE(gap[index + 1] == -gap[index]);
    previous = gap[index];
  }
  REQUIRE(gap.back() == 0.0F);
  REQUIRE(Fill(buffer, 1600, -16384, 16384));
  buffer.Render(gap);
  for (std::size_t index = 0; index < gap.size(); index += 2) {
    REQUIRE(std::abs(gap[index] - previous) <= 0.006F);
    REQUIRE(gap[index + 1] == -gap[index]);
    previous = gap[index];
  }
  REQUIRE(gap.front() > -0.01F);
  REQUIRE(gap.back() == 0.5F);
}

TEST_CASE("Host audio recovery stays bounded and flush resets its delay and fade", "[unit][audio_output]") {
  AudioBuffer buffer;
  Activate(buffer);
  const auto capacity = buffer.GetStats().capacity_frames;
  std::vector<float> drain((capacity + 320) * 2);
  for (int attempt = 0; attempt < 10; ++attempt) {
    REQUIRE(Fill(buffer, capacity));
    buffer.Render(drain);
    REQUIRE(drain[200] == 0.5F);  // Even repeated starvation can always re-prime.
    REQUIRE(drain.back() == 0.0F);
    REQUIRE(buffer.GetStats().queued_frames == 0);
  }
  buffer.Flush();
  REQUIRE(Fill(buffer, 640, -8192, 8192));  // Normal startup target is restored.
  std::array<float, 1282> output;
  buffer.Render(output);
  REQUIRE(output[0] == -0.25F);
  REQUIRE(output[1280] < 0.0F);  // The starvation tail has just begun.
  buffer.SetMuted(true);
  buffer.Render(output);
  REQUIRE(Silent(output));
  buffer.SetMuted(false);
  buffer.SetActive(false);
  buffer.Render(output);
  REQUIRE(Silent(output));
  buffer.SetActive(true);
  REQUIRE(Fill(buffer, 640, 8192, -8192));
  buffer.Render(output);
  REQUIRE(output[0] == 0.25F);
  buffer.Flush();  // Also cut an in-flight tail immediately, without leaking it.
  buffer.Render(output);
  REQUIRE(Silent(output));
}

TEST_CASE("Host audio starvation fades are independent of callback slicing", "[unit][audio_output]") {
  for (uint32_t rate : {8000U, 32000U, 44100U, 48000U, 96000U, 384000U}) {
    CAPTURE(rate);
    AudioBuffer whole;
    AudioBuffer sliced;
    Activate(whole, rate);
    Activate(sliced, rate);
    for (int pass = 0; pass < 3; ++pass) {
      const std::size_t count = pass == 0 ? 640 : 1800;
      REQUIRE(Fill(whole, count));
      REQUIRE(Fill(sliced, count));
      std::vector<float> expected(rate / 10 * 2);
      std::vector<float> actual(expected.size());
      whole.Render(expected);
      for (std::size_t offset = 0; offset < actual.size();) {
        const auto size = std::min(std::size_t{74}, actual.size() - offset);
        sliced.Render(std::span(actual).subspan(offset, size));
        offset += size;
      }
      REQUIRE(actual == expected);
      REQUIRE(whole.GetStats().underrun_frames == sliced.GetStats().underrun_frames);
    }
  }
}

TEST_CASE("Host gain and mute are bounded and muted playback consumes the queue", "[unit][audio_output]") {
  AudioBuffer buffer;
  Activate(buffer);
  REQUIRE(Fill(buffer, 640, INT16_MIN, INT16_MAX));
  std::array<float, 2> output;
  buffer.Render(output);
  REQUIRE(output[0] == -1.0F);
  REQUIRE(output[1] == 32767.0F / 32768.0F);
  buffer.SetVolume(0.5F);
  buffer.Render(output);
  REQUIRE(output[0] == -0.5F);
  REQUIRE(output[1] == 32767.0F / 65536.0F);
  const uint64_t before_mute = buffer.GetStats().queued_frames;
  buffer.SetMuted(true);
  buffer.Render(output);
  REQUIRE(Silent(output));
  REQUIRE(buffer.GetStats().queued_frames == before_mute - 1);
  buffer.SetMuted(false);
  for (float gain : {-1.0F, 2.0F, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    CAPTURE(gain);
    buffer.SetVolume(gain);
    buffer.Render(output);
    const float expected_gain = std::isfinite(gain) ? std::clamp(gain, 0.0F, 1.0F) : 0.5F;
    REQUIRE(output[0] == -expected_gain);
    REQUIRE(std::isfinite(output[1]));
  }
}

TEST_CASE("Host audio overflow drops new frames without replacing unread stereo data", "[unit][audio_output]") {
  AudioBuffer buffer;
  Activate(buffer, 32000, 20);
  bool accepted = true;
  const auto capacity = buffer.GetStats().capacity_frames;
  for (int16_t index = 0; index < static_cast<int16_t>(capacity); ++index)
    accepted &= buffer.PushSample(index, static_cast<int16_t>(-index));
  REQUIRE(accepted);
  REQUIRE_FALSE(Fill(buffer, 19, 30000, 30000));
  REQUIRE(buffer.GetStats().queued_frames == capacity);
  REQUIRE(buffer.GetStats().dropped_frames == 19);
  std::vector<float> output(capacity * 2);
  buffer.Render(output);
  bool intact = true;
  for (std::size_t index = 0; index < capacity; ++index) {
    const auto sample = static_cast<float>(index) / 32768.0F;
    intact &= output[index * 2] == sample && output[index * 2 + 1] == -sample;
  }
  REQUIRE(intact);
  REQUIRE(buffer.GetStats().queued_frames == 0);
  REQUIRE(Fill(buffer, capacity, 8192, -8192));
  buffer.Render(output);
  REQUIRE(output[0] == 0.25F);
  REQUIRE(output[1] == -0.25F);
  REQUIRE(buffer.GetStats().dropped_frames == 19);
}

TEST_CASE("Host audio flush discards interpolation history but preserves samples pushed after the cutoff",
          "[unit][audio_output]") {
  AudioBuffer buffer;
  Activate(buffer, 48000);
  REQUIRE(Fill(buffer, 640, 16384, -16384));
  std::array<float, 1918> previous;
  buffer.Render(previous);  // Consume the ring, retaining the fractional interpolation pair.
  REQUIRE(previous.front() == 0.5F);
  REQUIRE(previous.back() == -0.5F);
  buffer.Flush();
  REQUIRE(buffer.GetStats().queued_frames == 0);
  REQUIRE(Fill(buffer, 640, -8192, 8192));
  std::array<float, 4> output;
  buffer.Render(output);
  REQUIRE(output[0] == -0.25F);
  REQUIRE(output[1] == 0.25F);
  REQUIRE(output[2] == -0.25F);
  REQUIRE(output[3] == 0.25F);
  buffer.Flush();
  buffer.Flush();
  buffer.Render(output);
  REQUIRE(Silent(output));
}

TEST_CASE("Host playback transitions flush paused steps and repeated active updates retain audio",
          "[unit][audio_output]") {
  AudioBuffer buffer;
  Activate(buffer);
  REQUIRE(Fill(buffer, 640));
  buffer.SetActive(true);
  REQUIRE(buffer.GetStats().queued_frames == 640);
  buffer.SetActive(false);
  REQUIRE_FALSE(Fill(buffer, 200));
  std::array<float, 8> output;
  buffer.Render(output);
  REQUIRE(Silent(output));
  REQUIRE(buffer.GetStats().queued_frames == 0);
  buffer.SetActive(true);
  REQUIRE(Fill(buffer, 640, 4096, -4096));
  buffer.Render(output);
  REQUIRE(output[0] == 0.125F);
  REQUIRE(output[1] == -0.125F);
  REQUIRE(buffer.GetStats().dropped_frames == 0);
}

TEST_CASE("Host audio reconfiguration resets cached samples and validates clock and capacity", "[unit][audio_output]") {
  AudioBuffer buffer;
  Activate(buffer, 44100);
  REQUIRE(Fill(buffer, 640));
  std::array<float, 2> output;
  buffer.Render(output);
  buffer.Configure(0, 0);
  REQUIRE(buffer.GetStats().sample_rate == 48000);
  REQUIRE(buffer.GetStats().capacity_frames == 2816);
  REQUIRE(buffer.GetStats().queued_frames == 0);
  buffer.Render(output);  // Acknowledge cutoff before refilling the smaller queue.
  REQUIRE(Silent(output));
  REQUIRE(Fill(buffer, 640, -16384, 16384));
  buffer.Render(output);
  REQUIRE(output[0] == -0.5F);
  buffer.Configure(UINT32_MAX, UINT32_MAX);
  REQUIRE(buffer.GetStats().sample_rate == 48000);
  REQUIRE(buffer.GetStats().capacity_frames == 7296);
  REQUIRE(buffer.GetStats().queued_frames == 0);
}

TEST_CASE("Host audio SPSC callbacks survive repeated concurrent flushes without torn stereo frames",
          "[unit][audio_output]") {
  AudioBuffer buffer;
  Activate(buffer, 48000, 20);
  std::atomic<bool> finished{false};
  std::atomic<bool> invalid{false};
  std::atomic<uint64_t> callbacks{0};
  std::thread consumer([&] {
    std::array<float, 254> output;
    do {
      buffer.Render(output);
      for (std::size_t index = 0; index < output.size(); index += 2) {
        if (!std::isfinite(output[index]) || output[index] < -1.0F || output[index] > 1.0F ||
            output[index + 1] != -output[index]) {
          invalid.store(true, std::memory_order_relaxed);
        }
      }
      callbacks.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::yield();
    } while (!finished.load(std::memory_order_acquire));
  });
  for (uint32_t index = 0; index < 100000; ++index) {
    const auto sample = static_cast<int16_t>(index % 30000);
    static_cast<void>(buffer.PushSample(sample, static_cast<int16_t>(-sample)));
    if (index % 997 == 0) buffer.Flush();
    if (index % 2003 == 0) {
      buffer.SetActive(false);
      buffer.SetActive(true);
    }
    if (index % 1009 == 0) buffer.SetVolume(static_cast<float>(index % 101) / 100.0F);
    if (index % 1021 == 0) buffer.SetMuted((index & 1) != 0);
  }
  finished.store(true, std::memory_order_release);
  consumer.join();  // Callback storage is destroyed only after the consumer exits.
  REQUIRE(callbacks.load() > 0);
  REQUIRE_FALSE(invalid.load());
  REQUIRE(buffer.GetStats().queued_frames <= buffer.GetStats().capacity_frames);
  buffer.SetActive(false);
  std::array<float, 8> output;
  buffer.Render(output);
  REQUIRE(Silent(output));
}

TEST_CASE("Host audio owner is stable and disabled initialization and repeated shutdown need no device",
          "[unit][audio_output]") {
  static_assert(!std::is_copy_constructible_v<AudioOutput>);
  static_assert(!std::is_move_constructible_v<AudioOutput>);
  AudioOutput output;
  REQUIRE_FALSE(output.IsOpen());
  REQUIRE(output.GetSampleRate() == 0);
  REQUIRE(output.GetError().empty());
  REQUIRE(output.ApplySettings({.enabled = false, .volume = 0.75F, .device_id = {}}));
  REQUIRE(output.GetSettings().volume == 0.75F);
  output.SetPlaybackActive(true);
  output.PushSample(INT16_MAX, INT16_MIN);
  REQUIRE(output.GetStats().queued_frames == 0);
  REQUIRE(output.Retry());
  REQUIRE_FALSE(output.IsOpen());
  output.Flush();
  output.Shutdown();
  output.Shutdown();
  REQUIRE(output.GetError().empty());
  REQUIRE(output.GetStats().queued_frames == 0);
}
