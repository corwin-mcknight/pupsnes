#include <algorithm>
#include <cinttypes>
#include <cstring>

#include "debugger/app.h"
#include "imgui.h"
#include "panels.h"

namespace pupsnes::debugger {

void RenderTraceRecordPanel(DebuggerApp& app) {
  UiState& ui = app.GetUiState();
  if (!ui.show_trace_record_panel) {
    return;
  }
  if (!ImGui::Begin("Trace Record", &ui.show_trace_record_panel)) {
    ImGui::End();
    return;
  }

  const bool recording = app.IsTraceRecording();
  if (recording) {
    ImGui::TextColored(ImVec4(0.95F, 0.40F, 0.40F, 1.0F), "[REC]");
    ImGui::SameLine();
    ImGui::Text("%s", app.TraceRecordingPath().c_str());
    ImGui::Text("Lines: %" PRIu64, app.TraceRecordedLines());
  } else {
    ImGui::TextDisabled("(stopped)");
  }

  ImGui::Separator();

  char path_buf[512];
  const std::string& current = recording ? app.TraceRecordingPath() : ui.trace_record_path;
  const std::size_t copy_len = std::min(current.size(), sizeof(path_buf) - 1);
  std::memcpy(path_buf, current.c_str(), copy_len);
  path_buf[copy_len] = '\0';
  ImGui::BeginDisabled(recording);
  if (ImGui::InputText("Path", path_buf, sizeof(path_buf))) {
    ui.trace_record_path = path_buf;
  }
  ImGui::Checkbox("Reset ROM on Start", &ui.trace_record_reset_on_start);
  ImGui::EndDisabled();

  ImGui::Separator();

  ImGui::BeginDisabled(!app.HasLoadedRom() || recording);
  if (ImGui::Button("Start")) {
    (void)app.StartTraceRecording(ui.trace_record_path, ui.trace_record_reset_on_start);
  }
  ImGui::EndDisabled();

  ImGui::SameLine();
  ImGui::BeginDisabled(!recording);
  if (ImGui::Button("Stop")) {
    app.StopTraceRecording();
  }
  ImGui::SameLine();
  if (ImGui::Button("Flush")) {
    app.FlushTraceRecording();
  }
  ImGui::EndDisabled();

  if (!app.TraceLastError().empty()) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.95F, 0.25F, 0.25F, 1.0F), "Error: %s",
                       app.TraceLastError().c_str());
  }

  ImGui::End();
}

}  // namespace pupsnes::debugger
