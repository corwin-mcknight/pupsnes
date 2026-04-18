#include <cinttypes>

#include "debugger/app.h"
#include "imgui.h"
#include "panels.h"

namespace pupsnes::debugger {

void RenderTracePanel(DebuggerApp& app) {
  ImGui::Begin("Trace");
  const auto snapshot = app.GetTraceLog().Snapshot();
  UiState& ui = app.GetUiState();
  const bool grew = snapshot.size() != ui.trace_last_seen_size;
  ui.trace_last_seen_size = snapshot.size();

  if (ImGui::BeginTable("trace_table", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("PC", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Op", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (const TraceEntry& entry : snapshot) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%" PRIu64, entry.master_time);
      ImGui::TableSetColumnIndex(1);
      ImGui::Text("$%02X:%04X", static_cast<unsigned>(entry.pc >> 16), static_cast<unsigned>(entry.pc & 0xFFFFU));
      ImGui::TableSetColumnIndex(2);
      ImGui::Text("%02X", entry.opcode);
      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(entry.text.c_str());
    }

    if (grew) {
      ImGui::SetScrollHereY(1.0F);
    }
    ImGui::EndTable();
  }
  ImGui::End();
}

}  // namespace pupsnes::debugger
