#include "frontend/audio_panel.h"

#include <array>
#include <cstdio>

#include "imgui.h"
#include "pupsnes/core/snes.h"

namespace pupsnes::frontend {

bool RenderAudioMenu(AudioOutput& audio, AudioPanelState& panel) {
  const ImGuiIO& io = ImGui::GetIO();
  // ImGui remaps physical Cmd to semantic Ctrl when macOS behavior is on.
  const bool extra_modifier = io.KeyAlt || io.KeyShift || io.KeySuper;
  if (!io.WantTextInput && io.KeyCtrl && !extra_modifier && ImGui::IsKeyPressed(ImGuiKey_Comma, false)) {
    panel.show = true;
  }

  bool changed = false;
  if (ImGui::BeginMenu("Audio")) {
    AudioSettings settings = audio.GetSettings();
    changed |= ImGui::MenuItem("Enable output", nullptr, &settings.enabled);
    changed |= ImGui::MenuItem("Mute", nullptr, &settings.muted);
    ImGui::Separator();
    if (ImGui::MenuItem("Audio settings...", io.ConfigMacOSXBehaviors ? "Cmd+," : "Ctrl+,")) panel.show = true;
    if (!audio.GetError().empty()) ImGui::TextDisabled("Output unavailable - open settings");
    ImGui::EndMenu();
    if (changed) (void)audio.ApplySettings(settings);
  }
  return changed;
}

bool RenderAudioPanel(AudioOutput& audio, SNES& snes, AudioPanelState& panel, bool running, float speed_multiplier) {
  if (!panel.show) return false;
  ImGui::SetNextWindowSize(ImVec2(440.0F, 0.0F), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Audio", &panel.show, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return false;
  }
  if (!panel.devices_loaded) {
    panel.devices = audio.EnumerateDevices();
    panel.devices_loaded = true;
  }

  AudioSettings settings = audio.GetSettings();
  bool changed = ImGui::Checkbox("Enable output", &settings.enabled);
  ImGui::SameLine();
  changed |= ImGui::Checkbox("Mute", &settings.muted);
  float volume_percent = settings.volume * 100.0F;
  ImGui::SetNextItemWidth(260.0F);
  if (ImGui::SliderFloat("Volume", &volume_percent, 0.0F, 100.0F, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
    settings.volume = volume_percent / 100.0F;
    changed = true;
  }

  const char* device_label = settings.device_id.empty() ? "System default" : "Unavailable device (choose another)";
  for (const auto& device : panel.devices) {
    if (!settings.device_id.empty() && device.id == settings.device_id) device_label = device.name.c_str();
  }
  ImGui::SetNextItemWidth(300.0F);
  if (ImGui::BeginCombo("Output device", device_label)) {
    if (ImGui::Selectable("System default", settings.device_id.empty())) {
      settings.device_id.clear();
      changed = true;
    }
    for (const auto& device : panel.devices) {
      if (device.id.empty()) continue;
      ImGui::PushID(device.id.c_str());
      if (ImGui::Selectable(device.name.c_str(), settings.device_id == device.id)) {
        settings.device_id = device.id;
        changed = true;
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  if (ImGui::Button("Refresh devices")) panel.devices = audio.EnumerateDevices();
  ImGui::SameLine();
  if (ImGui::Button("Retry output")) (void)audio.Retry();

  struct LatencyPreset {
    uint32_t milliseconds;
    const char* label;
  };
  constexpr std::array<LatencyPreset, 4> kLatencies = {
      {{20, "20 ms - Low"}, {40, "40 ms - Balanced"}, {80, "80 ms - Stable"}, {160, "160 ms - Extra buffer"}}};
  char latency_label[32];
  std::snprintf(latency_label, sizeof(latency_label), "%u ms", settings.latency_ms);
  ImGui::SetNextItemWidth(220.0F);
  if (ImGui::BeginCombo("Latency", latency_label)) {
    for (const auto& preset : kLatencies) {
      if (ImGui::Selectable(preset.label, settings.latency_ms == preset.milliseconds)) {
        settings.latency_ms = preset.milliseconds;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::TextDisabled("Increase latency if sound breaks up.");

  ImGui::Separator();
  constexpr std::array<const char*, 2> kQualityLabels = {"Linear", "Gaussian (SNES)"};
  int pending_mode = static_cast<int>(snes.GetSdspModePending());
  ImGui::SetNextItemWidth(220.0F);
  const bool quality_changed = ImGui::Combo("Sound interpolation", &pending_mode, kQualityLabels.data(),
                                            static_cast<int>(kQualityLabels.size()));
  if (quality_changed) snes.SetSdspModePending(static_cast<SdspMode>(pending_mode));
  if (snes.GetSdspModePending() != snes.GetSdspModeLive()) ImGui::TextDisabled("Reset the game to apply this change.");

  if (changed) (void)audio.ApplySettings(settings);
  ImGui::Separator();
  if (!audio.GetError().empty()) {
    ImGui::TextWrapped("Audio output unavailable: %s", audio.GetError().c_str());
    ImGui::TextDisabled("Choose a device or retry. Emulation can continue.");
  } else if (!settings.enabled) {
    ImGui::TextDisabled("Output disabled.");
  } else if (settings.muted || settings.volume == 0.0F) {
    ImGui::TextDisabled("Muted.");
  } else if (!CanPlayAudio(true, speed_multiplier)) {
    ImGui::TextDisabled("Audio is muted at speeds other than 100%%.");
  } else if (!running) {
    ImGui::TextDisabled("Audio resumes when the game runs. Stepping is silent.");
  } else if (audio.IsOpen()) {
    ImGui::TextDisabled("Audio output active.");
  }
  ImGui::End();
  return changed || quality_changed;
}

}  // namespace pupsnes::frontend
