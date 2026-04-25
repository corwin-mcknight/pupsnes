#include <cstdio>

#include "debugger/app.h"
#include "imgui.h"
#include "panel_utils.h"
#include "panels.h"

namespace pupsnes::debugger {

namespace {

constexpr const char* kMemoryRegions[] = {"Bus", "WRAM", "ROM"};

}  // namespace

void RenderMemoryPanel(DebuggerApp& app) {
  UiState& ui = app.GetUiState();
  ScopedPanel panel("Memory", ui.show_memory_panel);
  if (!panel) return;
  if (!app.HasLoadedRom()) {
    ImGui::TextUnformatted("Load a ROM to inspect memory.");
    return;
  }

  if (ImGui::Combo("Region", &ui.memory_region, kMemoryRegions, IM_ARRAYSIZE(kMemoryRegions))) {
    if (ui.memory_region == 1) {
      ui.memory_address = 0x7E0000;
    } else if (ui.memory_region == 2) {
      ui.memory_address = 0x008000;
    }
  }
  ImGui::SetNextItemWidth(160.0F);
  ImGui::InputScalar("Base", ImGuiDataType_U32, &ui.memory_address, nullptr, nullptr, "%06X",
                     ImGuiInputTextFlags_CharsHexadecimal);

  const SnesAddrT base = ui.memory_address & 0x00FFFFF0U;
  ImGui::Separator();
  for (int row = 0; row < 16; ++row) {
    const SnesAddrT row_addr = (base + static_cast<SnesAddrT>(row * 16)) & 0x00FFFFFFU;
    TextAddress24(row_addr);
    ImGui::SameLine();

    for (int col = 0; col < 16; ++col) {
      const SnesAddrT addr = (row_addr + static_cast<SnesAddrT>(col)) & 0x00FFFFFFU;
      const DebugReadResult cell = app.GetSnes().GetSystemBus().DebugRead(addr);
      const uint8_t value = cell.ok ? cell.value : 0xFFU;
      char label[4] = {};
      if (cell.ok) {
        std::snprintf(label, sizeof(label), "%02X", value);
      }

      ImGui::PushID(static_cast<int>(addr));
      if (ImGui::SmallButton(cell.ok ? label : "??")) {
        ui.selected_memory_address = addr;
        ui.memory_edit_value = value;
      }
      ImGui::PopID();

      if (col != 15) {
        ImGui::SameLine();
      }
    }
  }

  if (ui.selected_memory_address.has_value()) {
    ImGui::Separator();
    ImGui::Text("Selected %s", FormatAddress24(*ui.selected_memory_address).c_str());
    ImGui::SetNextItemWidth(80.0F);
    ImGui::InputScalar("Value", ImGuiDataType_U8, &ui.memory_edit_value, nullptr, nullptr, "%02X",
                       ImGuiInputTextFlags_CharsHexadecimal);
    if (ImGui::Button("Write")) {
      (void)app.WriteMemory(*ui.selected_memory_address, ui.memory_edit_value);
    }
  }
}

}  // namespace pupsnes::debugger
