#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "debugger/app.h"
#include "imgui.h"
#include "panel_utils.h"
#include "panels.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/core/device.h"
#include "pupsnes/hw/apu/sdsp.h"
#include "pupsnes/core/snes.h"

namespace pupsnes::debugger {

namespace {

constexpr float kMasterClockMHz = 21.477272F;

// SDSP mode combo entries. Order matches the SdspMode enum so the index can be
// reinterpreted directly. Update both together if a third backend lands.
constexpr const char* kSdspModeLabels[] = {"Simple", "Accurate"};

[[nodiscard]] const char* MapperLabel(MapperKind kind) {
  switch (kind) {
    case MapperKind::kLoROM: return "LoROM";
    case MapperKind::kHiROM: return "HiROM";
    case MapperKind::kExHiROM: return "ExHiROM";
    case MapperKind::kNone: return "None";
  }
  return "?";
}

// SNES country byte → short region label. Strictly informational; the
// emulator itself runs NTSC timing regardless of this value today.
[[nodiscard]] const char* CountryLabel(uint8_t code) {
  switch (code) {
    case 0x00: return "Japan (NTSC)";
    case 0x01: return "USA (NTSC)";
    case 0x02: return "Europe (PAL)";
    case 0x03: return "Sweden / Scandinavia (PAL)";
    case 0x04: return "Finland (PAL)";
    case 0x05: return "Denmark (PAL)";
    case 0x06: return "France (PAL)";
    case 0x07: return "Netherlands (PAL)";
    case 0x08: return "Spain (PAL)";
    case 0x09: return "Germany (PAL)";
    case 0x0A: return "Italy (PAL)";
    case 0x0B: return "China (PAL)";
    case 0x0C: return "Indonesia (PAL)";
    case 0x0D: return "Korea (NTSC)";
    case 0x0F: return "Canada (NTSC)";
    case 0x10: return "Brazil (PAL-M)";
    case 0x11: return "Australia (PAL)";
    default: return "Unknown";
  }
}

void FormatByteSize(char* buf, std::size_t buf_size, std::size_t bytes) {
  if (bytes == 0) {
    std::snprintf(buf, buf_size, "0");
  } else if (bytes >= 1024U * 1024U) {
    std::snprintf(buf, buf_size, "%zu MiB", bytes / (1024U * 1024U));
  } else if (bytes >= 1024U) {
    std::snprintf(buf, buf_size, "%zu KiB", bytes / 1024U);
  } else {
    std::snprintf(buf, buf_size, "%zu B", bytes);
  }
}

void LabelValue(const char* label, std::string_view value) {
  ImGui::TextDisabled("%s", label);
  ImGui::SameLine();
  ImGui::TextUnformatted(value.data(), value.data() + value.size());
}

// Two-step variant for callers that want printf-style formatting on the value
// side. Inlined at each call site because -Wformat-nonliteral forbids
// trampolining the format string through a helper.
void LabelStart(const char* label) {
  ImGui::TextDisabled("%s", label);
  ImGui::SameLine();
}

void RenderCartridgeSection(const Cartridge& cart, std::string_view rom_path) {
  if (cart.GetMapperKind() == MapperKind::kNone) {
    ImGui::TextDisabled("No ROM loaded.");
    return;
  }

  char rom_size_buf[32];
  char sram_size_buf[32];
  FormatByteSize(rom_size_buf, sizeof(rom_size_buf), cart.Size());
  FormatByteSize(sram_size_buf, sizeof(sram_size_buf), cart.SramSize());

  LabelValue("File:", rom_path.empty() ? std::string_view{"(none)"} : rom_path);
  LabelValue("Title:", cart.GetInternalTitle());
  LabelValue("Mapper:", MapperLabel(cart.GetMapperKind()));
  LabelValue("ROM size:", rom_size_buf);
  LabelValue("SRAM size:", sram_size_buf);
  LabelValue("FastROM:", cart.IsFastRomCapable() ? "capable" : "no");
  const uint8_t country = cart.GetCountryCode();
  LabelStart("Country:");
  ImGui::Text("$%02X \xe2\x80\x94 %s", country, CountryLabel(country));
}

void RenderSystemSection() {
  // Region is hardcoded NTSC in the emulator today (no PAL timing path yet).
  // Surfacing the field reserves the slot.
  LabelValue("Region:", "NTSC (fixed)");
  LabelStart("Master clock:");
  ImGui::Text("%.6f MHz", static_cast<double>(kMasterClockMHz));
}

void RenderPeripheralsSection() {
  if (ImGui::BeginTable("##peripherals", 2, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("Controller 1");
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("Standard Joypad");

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("Controller 2");
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("None");

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("Cartridge slot");
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("Cartridge");

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("Expansion port");
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("None");

    ImGui::EndTable();
  }
}

void RenderApuSection(SNES& snes) {
  const SdspMode live = snes.GetSdspModeLive();
  int pending_index = static_cast<int>(snes.GetSdspModePending());
  ImGui::TextDisabled("S-DSP mode");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(140.0F);
  if (ImGui::Combo("##sdsp_mode", &pending_index, kSdspModeLabels,
                   IM_ARRAYSIZE(kSdspModeLabels))) {
    snes.SetSdspModePending(static_cast<SdspMode>(pending_index));
  }
  if (snes.GetSdspModePending() != live) {
    ImGui::SameLine();
    ImGui::TextDisabled("(reset to apply)");
  }
  ImGui::TextDisabled("Live: %s", kSdspModeLabels[static_cast<int>(live)]);
}

void RenderInternalDevicesSection(const SNES& snes) {
  if (ImGui::BeginTable("##devices", 2,
                        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("ID");
    ImGui::TableHeadersRow();
    const std::size_t count = snes.GetDeviceCount();
    for (std::size_t i = 0; i < count; ++i) {
      const Device* dev = snes.GetDevice(static_cast<DeviceIdT>(i));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(dev != nullptr ? dev->DeviceName() : "?");
      ImGui::TableNextColumn();
      ImGui::Text("%u", static_cast<unsigned>(i));
    }
    ImGui::EndTable();
  }
}

}  // namespace

void RenderSnesPanel(DebuggerApp& app) {
  ScopedPanel panel("SNES", app.GetUiState().show_snes_panel);
  if (!panel) return;

  SNES& snes = app.GetSnes();

  if (ImGui::CollapsingHeader("Cartridge", ImGuiTreeNodeFlags_DefaultOpen)) {
    RenderCartridgeSection(snes.GetCartridge(), app.GetLoadedRomPath());
  }
  if (ImGui::CollapsingHeader("System", ImGuiTreeNodeFlags_DefaultOpen)) {
    RenderSystemSection();
  }
  if (ImGui::CollapsingHeader("Peripherals", ImGuiTreeNodeFlags_DefaultOpen)) {
    RenderPeripheralsSection();
  }
  if (ImGui::CollapsingHeader("APU", ImGuiTreeNodeFlags_DefaultOpen)) {
    RenderApuSection(snes);
  }
  if (ImGui::CollapsingHeader("Internal devices")) {
    RenderInternalDevicesSection(snes);
  }

  ImGui::Dummy(ImVec2(0.0F, 8.0F));
  ImGui::Separator();
  const bool has_rom = app.HasLoadedRom();
  if (!has_rom) ImGui::BeginDisabled();
  if (ImGui::Button("Reset SNES")) {
    app.ResetMachine();
  }
  if (!has_rom) ImGui::EndDisabled();
}

}  // namespace pupsnes::debugger
