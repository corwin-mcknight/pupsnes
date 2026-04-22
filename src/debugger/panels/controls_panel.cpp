#include <cinttypes>

#include "debugger/app.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "panels.h"
#include "pupsnes/hw/5a22/cpu_mmio.h"
#include "pupsnes/hw/cartridge.h"

namespace pupsnes::debugger {

namespace {

const char* RunStateLabel(RunState state) {
  switch (state) {
    case RunState::kPaused: return "Paused";
    case RunState::kStepOne: return "Step One";
    case RunState::kStepN: return "Step N";
    case RunState::kRunUntilBreak: return "Running";
  }
  return "Unknown";
}

const char* MapperLabel(MapperKind kind) {
  switch (kind) {
    case MapperKind::kNone: return "—";
    case MapperKind::kLoROM: return "LoROM";
    case MapperKind::kHiROM: return "HiROM";
  }
  return "?";
}

}  // namespace

void RenderControlsPanel(DebuggerApp& app) {
  UiState& ui = app.GetUiState();

  const float height = ImGui::GetFrameHeight() + (ImGui::GetStyle().WindowPadding.y * 2.0F);
  const bool visible = ImGui::BeginViewportSideBar("##ControlsToolbar", ImGui::GetMainViewport(), ImGuiDir_Up, height,
                                                   ImGuiWindowFlags_NoSavedSettings);
  if (visible) {
    ImGui::BeginDisabled(!app.HasLoadedRom());
    if (ImGui::Button("Reset")) {
      app.ResetMachine();
    }
    ImGui::SameLine();
    if (ImGui::Button("Pause")) {
      app.GetRunControl().Pause();
    }
    ImGui::SameLine();
    if (ImGui::Button("Step")) {
      app.GetRunControl().RequestStepOne();
    }
    ImGui::SameLine();
    if (ImGui::Button("Step μop")) {
      app.GetRunControl().RequestStepOne(StepGranularity::kMicroOp);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0F);
    ImGui::InputScalar("##StepCount", ImGuiDataType_U64, &ui.step_count);
    ImGui::SameLine();
    if (ImGui::Button("Step N")) {
      app.GetRunControl().RequestStepN(ui.step_count);
    }
    ImGui::SameLine();
    if (ImGui::Button("Run")) {
      app.GetRunControl().RequestRunUntilBreak();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    ImGui::Text("State: %s", RunStateLabel(app.GetRunControl().GetState()));
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    ImGui::Text("Retired: %" PRIu64, app.GetSnes().GetCpu().GetRetiredInstructionCount());

    if (app.HasLoadedRom()) {
      ImGui::SameLine();
      ImGui::TextUnformatted("|");
      ImGui::SameLine();
      const std::string_view rom_path = app.GetLoadedRomPath();
      ImGui::TextUnformatted("ROM:");
      ImGui::SameLine();
      ImGui::TextUnformatted(rom_path.data(), rom_path.data() + rom_path.size());

      ImGui::SameLine();
      ImGui::TextUnformatted("|");
      ImGui::SameLine();
      const MapperKind mapper = app.GetSnes().GetCartridge().GetMapperKind();
      ImGui::Text("Mapper: %s", MapperLabel(mapper));

      ImGui::SameLine();
      const bool fast = app.GetSnes().GetCpuMmio().IsFastRomEnabled();
      // Dim label when FASTROM is off so the toolbar still reads "FASTROM" at
      // a glance without screaming when it's inactive.
      if (fast) {
        ImGui::TextColored(ImVec4(0.45F, 0.85F, 0.45F, 1.0F), "FASTROM");
      } else {
        ImGui::TextDisabled("FASTROM");
      }
    }

    if (const ErrorEvent* latest = app.GetErrorLog().Latest(); latest != nullptr) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.95F, 0.25F, 0.25F, 1.0F), "| %s", latest->message.c_str());
    }
  }

  ImGui::End();
}

}  // namespace pupsnes::debugger
