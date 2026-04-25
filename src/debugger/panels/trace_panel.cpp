#include <string>

#include "debugger/app.h"
#include "debugger/time_format.h"
#include "imgui.h"
#include "panel_utils.h"
#include "panels.h"
#include "pupsnes/debugger/disasm.h"

namespace pupsnes::debugger {

void RenderTracePanel(DebuggerApp& app) {
  UiState& ui = app.GetUiState();
  ScopedPanel panel("Trace", ui.show_trace_panel);
  if (!panel) return;
  const auto snapshot = app.GetTraceLog().Snapshot();
  const bool grew = snapshot.size() != ui.trace_last_seen_size;
  ui.trace_last_seen_size = snapshot.size();

  if (ImGui::BeginTable("trace_table", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("PC", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Op", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    const SNES& snes = app.GetSnes();
    const TimeMasterT now = snes.GetMasterTime();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(snapshot.size()));
    while (clipper.Step()) {
      for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
        const TraceEntry& entry = snapshot[static_cast<std::size_t>(row)];
        const DisassembledInstruction view = DisassembleInstructionRaw(snes, entry.pc, entry.regs.P);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        TextMasterTime(entry.master_time, now, ui.time_display_mode);
        ImGui::TableSetColumnIndex(1);
        TextAddress24(entry.pc);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%02X", view.opcode);
        ImGui::TableSetColumnIndex(3);
        const std::string text = FormatDisassembly(view);
        ImGui::TextUnformatted(text.c_str());
      }
    }

    if (grew) {
      ImGui::SetScrollHereY(1.0F);
    }
    ImGui::EndTable();
  }
}

}  // namespace pupsnes::debugger
