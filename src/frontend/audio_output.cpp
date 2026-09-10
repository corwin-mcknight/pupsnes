#include "frontend/audio_output.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <span>
#include <string_view>
#include <utility>

#include "pupsnes_miniaudio.h"

namespace pupsnes::frontend {
namespace {

std::string HexBytes(std::span<const unsigned char> bytes) {
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (unsigned char byte : bytes) {
    result += kHex[byte >> 4];
    result += kHex[byte & 15];
  }
  return result;
}

template <std::size_t Size>
std::string HexString(const char (&value)[Size]) {
  const auto end = std::find(value, value + Size, '\0');
  return HexBytes({reinterpret_cast<const unsigned char*>(value), static_cast<std::size_t>(end - value)});
}

// Persist the native identifier, not its enumeration index or unused union
// storage. Hex encoding keeps every backend's key safe for an INI value.
std::string DeviceKey(ma_backend backend, const ma_device_info& info) {
  std::string value;
  switch (backend) {
    case ma_backend_wasapi:
      for (auto unit : info.id.wasapi) {
        if (unit == 0) break;
        const unsigned char bytes[] = {static_cast<unsigned char>(unit >> 8), static_cast<unsigned char>(unit)};
        value += HexBytes(bytes);
      }
      break;
    case ma_backend_dsound: value = HexBytes(info.id.dsound); break;
    case ma_backend_winmm: value = std::to_string(info.id.winmm); break;
    case ma_backend_alsa: value = HexString(info.id.alsa); break;
    case ma_backend_pulseaudio: value = HexString(info.id.pulse); break;
    case ma_backend_jack: value = std::to_string(info.id.jack); break;
    case ma_backend_coreaudio: value = HexString(info.id.coreaudio); break;
    case ma_backend_sndio: value = HexString(info.id.sndio); break;
    case ma_backend_audio4: value = HexString(info.id.audio4); break;
    case ma_backend_oss: value = HexString(info.id.oss); break;
    case ma_backend_aaudio: value = std::to_string(info.id.aaudio); break;
    case ma_backend_opensl: value = std::to_string(info.id.opensl); break;
    case ma_backend_webaudio: value = HexString(info.id.webaudio); break;
    default: value = HexString(info.name); break;
  }
  return std::string(ma_get_backend_name(backend)) + ":" + value;
}

std::string DeviceError(std::string_view action, ma_result result) {
  return std::string(action) + ": " + ma_result_description(result);
}

}  // namespace

AudioSettings NormalizeAudioSettings(AudioSettings settings) {
  settings.volume = std::isfinite(settings.volume) ? std::clamp(settings.volume, 0.0F, 1.0F) : 0.5F;
  settings.latency_ms = std::clamp(settings.latency_ms, 20U, 160U);
  return settings;
}

struct AudioOutput::Impl {
  AudioBuffer buffer;
  ma_context context{};
  // Native C storage is filled by ma_device_init before it is used.
  // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
  ma_device device{};
  AudioSettings settings;
  std::string error;
  bool context_initialized = false;
  bool device_initialized = false;
  bool playback_requested = false;
  bool attempted_init = false;
  std::atomic<bool> closing{true};
  std::atomic<bool> device_lost{false};

  static void DataCallback(ma_device* native_device, void* output, const void*, ma_uint32 frames) {
    auto& self = *static_cast<Impl*>(native_device->pUserData);
    const std::span<float> samples(static_cast<float*>(output), static_cast<std::size_t>(frames) * 2);
    if (self.device_lost.load(std::memory_order_relaxed)) {
      std::fill(samples.begin(), samples.end(), 0.0F);
    } else {
      self.buffer.Render(samples);
    }
  }

  static void NotificationCallback(const ma_device_notification* notification) {
    auto& self = *static_cast<Impl*>(notification->pDevice->pUserData);
    if (!self.closing.load(std::memory_order_acquire) &&
        (notification->type == ma_device_notification_type_stopped ||
         notification->type == ma_device_notification_type_interruption_began)) {
      self.device_lost.store(true, std::memory_order_release);
    }
  }

  bool EnsureContext() {
    if (context_initialized) return true;
    const ma_result result = ma_context_init(nullptr, 0, nullptr, &context);
    if (result != MA_SUCCESS) {
      error = DeviceError("Audio output unavailable", result);
      return false;
    }
    context_initialized = true;
    return true;
  }

  void CloseDevice() {
    buffer.SetActive(false);
    buffer.Flush();
    closing.store(true, std::memory_order_release);
    if (device_initialized) {
      // miniaudio's uninit stops the device and joins its callbacks. The
      // context, callback state and ring buffer all outlive this call.
      ma_device_uninit(&device);
      device_initialized = false;
    }
    device_lost.store(false, std::memory_order_relaxed);
  }

  bool OpenDevice() {
    attempted_init = true;
    CloseDevice();
    if (!EnsureContext()) return false;

    ma_device_id selected{};
    if (!settings.device_id.empty()) {
      ma_device_info* devices = nullptr;
      ma_uint32 count = 0;
      const ma_result result = ma_context_get_devices(&context, &devices, &count, nullptr, nullptr);
      if (result != MA_SUCCESS) {
        error = DeviceError("Cannot enumerate audio outputs", result);
        return false;
      }
      bool found = false;
      for (ma_uint32 index = 0; index < count; ++index) {
        if (DeviceKey(context.backend, devices[index]) == settings.device_id) {
          selected = devices[index].id;
          found = true;
          break;
        }
      }
      if (!found) {
        error = "Selected audio output is unavailable. Choose another device or retry.";
        return false;
      }
    }

    // Low-level playback only: no engine, decoder, capture or microphone.
    // https://miniaud.io/docs/manual/index.html#LowLevelAPI
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.pDeviceID = settings.device_id.empty() ? nullptr : &selected;
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = 0;  // Resample our 32 kHz stream to the native rate.
    config.periodSizeInMilliseconds = settings.latency_ms / 2;
    config.periods = 2;
    config.dataCallback = &DataCallback;
    config.notificationCallback = &NotificationCallback;
    config.pUserData = this;
    ma_result result = ma_device_init(&context, &config, &device);
    if (result != MA_SUCCESS) {
      error = DeviceError("Cannot open audio output", result);
      return false;
    }
    device_initialized = true;
    buffer.Configure(device.sampleRate, settings.latency_ms);
    buffer.SetVolume(settings.volume);
    buffer.SetMuted(settings.muted);
    buffer.SetActive(playback_requested);
    closing.store(false, std::memory_order_release);
    result = ma_device_start(&device);
    if (result != MA_SUCCESS) {
      CloseDevice();
      error = DeviceError("Cannot start audio output", result);
      return false;
    }
    error.clear();
    return true;
  }
};

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}
AudioOutput::~AudioOutput() { Shutdown(); }

bool AudioOutput::ApplySettings(const AudioSettings& settings) {
  AudioSettings normalized = NormalizeAudioSettings(settings);
  const bool reopen = normalized.enabled != impl_->settings.enabled ||
                      normalized.device_id != impl_->settings.device_id ||
                      normalized.latency_ms != impl_->settings.latency_ms || !impl_->attempted_init;
  impl_->settings = std::move(normalized);
  impl_->buffer.SetVolume(impl_->settings.volume);
  impl_->buffer.SetMuted(impl_->settings.muted);
  if (!impl_->settings.enabled) {
    impl_->CloseDevice();
    impl_->error.clear();
    return true;
  }
  return reopen ? impl_->OpenDevice() : IsOpen();
}

bool AudioOutput::Retry() {
  if (!impl_->settings.enabled) return ApplySettings(impl_->settings);
  // Recreate the context too, so retry can recover a failed backend/server.
  impl_->CloseDevice();
  if (impl_->context_initialized) {
    ma_context_uninit(&impl_->context);
    impl_->context_initialized = false;
  }
  return impl_->OpenDevice();
}

void AudioOutput::Shutdown() {
  impl_->playback_requested = false;
  impl_->CloseDevice();
  if (impl_->context_initialized) {
    ma_context_uninit(&impl_->context);
    impl_->context_initialized = false;
  }
  impl_->attempted_init = false;
  impl_->error.clear();
}

std::vector<AudioDevice> AudioOutput::EnumerateDevices() {
  std::vector<AudioDevice> result{{"", "System default", true}};
  if (!impl_->EnsureContext()) return result;
  ma_device_info* devices = nullptr;
  ma_uint32 count = 0;
  const ma_result status = ma_context_get_devices(&impl_->context, &devices, &count, nullptr, nullptr);
  if (status != MA_SUCCESS) {
    impl_->error = DeviceError("Cannot enumerate audio outputs", status);
    return result;
  }
  result.reserve(static_cast<std::size_t>(count) + 1);
  for (ma_uint32 index = 0; index < count; ++index) {
    result.push_back(
        {DeviceKey(impl_->context.backend, devices[index]), devices[index].name, devices[index].isDefault != 0});
  }
  return result;
}

const AudioSettings& AudioOutput::GetSettings() const { return impl_->settings; }

bool AudioOutput::IsOpen() const {
  return impl_->device_initialized && !impl_->device_lost.load(std::memory_order_acquire);
}

const std::string& AudioOutput::GetError() const {
  static const std::string kDeviceLost = "Audio output stopped or was disconnected. Retry to reconnect.";
  return impl_->device_lost.load(std::memory_order_acquire) ? kDeviceLost : impl_->error;
}

uint32_t AudioOutput::GetSampleRate() const { return IsOpen() ? impl_->device.sampleRate : 0; }

AudioStats AudioOutput::GetStats() const {
  AudioStats result = impl_->buffer.GetStats();
  result.sample_rate = GetSampleRate();
  return result;
}

void AudioOutput::SetPlaybackActive(bool active) {
  impl_->playback_requested = active;
  impl_->buffer.SetActive(active && impl_->settings.enabled && IsOpen());
}

void AudioOutput::Flush() { impl_->buffer.Flush(); }

void AudioOutput::PushSample(int16_t left, int16_t right) noexcept {
  if (impl_->settings.enabled && impl_->playback_requested && IsOpen()) {
    static_cast<void>(impl_->buffer.PushSample(left, right));
  }
}

}  // namespace pupsnes::frontend
