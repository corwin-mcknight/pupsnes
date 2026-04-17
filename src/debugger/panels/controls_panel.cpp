#include <cinttypes>

#include "debugger/app.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "panels.h"

namespace pupsnes::debugger {

namespace {

const char* RunStateLabel(RunState state) {
  switch (state) {
    case RunState::kPaused:
      return "Paused";
    case RunState::kStepOne:
      return "Step One";
    case RunState::kStepN:
      return "Step N";
    case RunState::kRunUntilBreak:
      return "Running";
  }
  return "Unknown";
}

}  // namespace

void RenderControlsPanel(DebuggerApp& app) {
  UiState& ui = app.GetUiState();
  ImGui::Begin("Controls");

  ImGui::InputText("ROM Path", &ui.rom_path_input);
  ImGui::SameLine();
  if (ImGui::Button("Load ROM")) {
    (void)app.LoadRomFromPath(ui.rom_path_input);
  }

  if (app.HasLoadedRom()) {
    const std::string_view rom_path = app.GetLoadedRomPath();
    ImGui::TextUnformatted(rom_path.data(), rom_path.data() + rom_path.size());
  } else {
    ImGui::TextUnformatted("No ROM loaded");
  }

  ImGui::Separator();
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
  ImGui::SetNextItemWidth(120.0F);
  ImGui::InputScalar("Step Count", ImGuiDataType_U64, &ui.step_count);
  if (ImGui::Button("Step N")) {
    app.GetRunControl().RequestStepN(ui.step_count);
  }
  ImGui::SameLine();
  if (ImGui::Button("Run")) {
    app.GetRunControl().RequestRunUntilBreak();
  }
  ImGui::EndDisabled();

  ImGui::Separator();
  ImGui::Text("State: %s", RunStateLabel(app.GetRunControl().GetState()));
  ImGui::Text("Master Time: %" PRIu64, app.GetSnes().GetMasterTime());
  ImGui::Text("Retired Instructions: %" PRIu64, app.GetSnes().GetCpu().GetRetiredInstructionCount());

  if (const ErrorEvent* latest = app.GetErrorLog().Latest(); latest != nullptr) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.95F, 0.25F, 0.25F, 1.0F), "%s", latest->message.c_str());
  }

  ImGui::End();
}

}  // namespace pupsnes::debugger
