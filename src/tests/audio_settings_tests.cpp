#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

#include "frontend/audio_panel.h"

namespace {
using pupsnes::frontend::AudioSettings;
using pupsnes::frontend::CanPlayAudio;
using pupsnes::frontend::ParseAudioSetting;
using pupsnes::frontend::ParseFiniteSetting;
using pupsnes::frontend::ParseSoundQuality;
using pupsnes::frontend::WriteAudioSettings;
}  // namespace

TEST_CASE("Playback is silent while paused, stepping, or running at another speed", "[unit][audio][config]") {
  REQUIRE(CanPlayAudio(true, 1.0F));
  REQUIRE_FALSE(CanPlayAudio(false, 1.0F));
  REQUIRE_FALSE(CanPlayAudio(true, 0.5F));
  REQUIRE_FALSE(CanPlayAudio(true, 2.0F));
  REQUIRE_FALSE(CanPlayAudio(true, std::numeric_limits<float>::quiet_NaN()));
  REQUIRE_FALSE(CanPlayAudio(true, std::numeric_limits<float>::infinity()));
}

TEST_CASE("Audio preferences validate complete finite values before applying them", "[unit][audio][config]") {
  AudioSettings settings;
  REQUIRE(settings.enabled);
  REQUIRE_FALSE(settings.muted);
  REQUIRE(settings.volume == 0.5F);

  for (const std::string_view invalid : {"", "nan", "inf", "-inf", "1e100", "0.2junk", "0.2  "}) {
    INFO("Invalid volume: " << invalid);
    REQUIRE(ParseAudioSetting(settings, "audio_volume", invalid));
    REQUIRE(settings.volume == 0.5F);
  }
  REQUIRE(ParseAudioSetting(settings, "audio_volume", "-3"));
  REQUIRE(settings.volume == 0.0F);
  REQUIRE(ParseAudioSetting(settings, "audio_volume", "2"));
  REQUIRE(settings.volume == 1.0F);
  REQUIRE(ParseAudioSetting(settings, "audio_volume", "0.25"));
  REQUIRE(settings.volume == 0.25F);

  REQUIRE(ParseAudioSetting(settings, "audio_enabled", "false"));
  REQUIRE(settings.enabled);
  REQUIRE(ParseAudioSetting(settings, "audio_enabled", "0"));
  REQUIRE_FALSE(settings.enabled);
  REQUIRE(ParseAudioSetting(settings, "audio_muted", "1"));
  REQUIRE(settings.muted);
  REQUIRE_FALSE(ParseAudioSetting(settings, "unrelated_preference", "1"));
}

TEST_CASE("Audio latency, device IDs, and sound quality reject malformed config", "[unit][audio][config]") {
  AudioSettings settings;
  REQUIRE(ParseAudioSetting(settings, "audio_latency_ms", "80"));
  REQUIRE(settings.latency_ms == 80);
  for (const std::string_view invalid : {"", "nan", "inf", "40ms", "40.5", "99999999999999999999"}) {
    REQUIRE(ParseAudioSetting(settings, "audio_latency_ms", invalid));
    REQUIRE(settings.latency_ms == 80);
  }
  REQUIRE(ParseAudioSetting(settings, "audio_latency_ms", "-1"));
  REQUIRE(settings.latency_ms == 20);
  REQUIRE(ParseAudioSetting(settings, "audio_latency_ms", "9999"));
  REQUIRE(settings.latency_ms == 160);
  REQUIRE(ParseAudioSetting(settings, "audio_device", "stable-device-id"));
  REQUIRE(ParseAudioSetting(settings, "audio_device", "bad\nvalue"));
  REQUIRE(settings.device_id == "stable-device-id");
  REQUIRE(ParseAudioSetting(settings, "audio_device", ""));
  REQUIRE(settings.device_id.empty());

  REQUIRE(ParseSoundQuality("0") == pupsnes::SdspMode::kSimple);
  REQUIRE(ParseSoundQuality("1") == pupsnes::SdspMode::kAccurate);
  for (const std::string_view invalid : {"", "garbage", "2", "1suffix", "-1", "nan"}) {
    REQUIRE_FALSE(ParseSoundQuality(invalid).has_value());
  }
  REQUIRE_FALSE(ParseFiniteSetting("nan", 0.001F, 10.0F).has_value());
  REQUIRE_FALSE(ParseFiniteSetting("inf", 0.001F, 10.0F).has_value());
  REQUIRE(ParseFiniteSetting("20", 0.001F, 10.0F) == 10.0F);
}

TEST_CASE("Audio preferences round trip through the frontend INI format", "[unit][audio][config]") {
  AudioSettings saved;
  saved.enabled = false;
  saved.muted = true;
  saved.volume = 0.75F;
  saved.latency_ms = 80;
  saved.device_id = "device:123=456";
  std::stringstream stream;
  WriteAudioSettings(stream, saved);
  AudioSettings loaded;
  std::string line;
  while (std::getline(stream, line)) {
    const auto separator = line.find('=');
    REQUIRE(separator != std::string::npos);
    REQUIRE(ParseAudioSetting(loaded, line.substr(0, separator), line.substr(separator + 1)));
  }
  REQUIRE(loaded.enabled == saved.enabled);
  REQUIRE(loaded.muted == saved.muted);
  REQUIRE(loaded.volume == saved.volume);
  REQUIRE(loaded.latency_ms == saved.latency_ms);
  REQUIRE(loaded.device_id == saved.device_id);
}
