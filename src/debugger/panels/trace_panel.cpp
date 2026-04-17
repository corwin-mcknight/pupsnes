#include <cinttypes>

#include "debugger/app.h"
#include "imgui.h"
#include "panels.h"

namespace pupsnes::debugger {

void RenderTracePanel(DebuggerApp& app) {
  ImGui::Begin("Trace");
  if (ImGui::BeginTable("trace_table", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupColumn("Time");
    ImGui::TableSetupColumn("PC");
    ImGui::TableSetupColumn("Op");
    ImGui::TableSetupColumn("Text");
    ImGui::TableHeadersRow();

    for (const TraceEntry& entry : app.GetTraceLog().Snapshot()) {
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
    ImGui::EndTable();
  }
  ImGui::End();
}

}  // namespace pupsnes::debugger
