#include <cinttypes>

#include "debugger/app.h"
#include "imgui.h"
#include "panels.h"

namespace pupsnes::debugger {

namespace {

const char* SeverityLabel(ErrorSeverity severity) {
  switch (severity) {
    case ErrorSeverity::kInfo: return "Info";
    case ErrorSeverity::kWarning: return "Warn";
    case ErrorSeverity::kError: return "Error";
    case ErrorSeverity::kFatal: return "Fatal";
  }
  return "?";
}

const char* SourceLabel(ErrorSource source) {
  switch (source) {
    case ErrorSource::kCpu: return "CPU";
    case ErrorSource::kBus: return "Bus";
    case ErrorSource::kScheduler: return "Scheduler";
    case ErrorSource::kRomLoader: return "ROM";
    case ErrorSource::kHost: return "Host";
  }
  return "?";
}

}  // namespace

void RenderErrorsPanel(DebuggerApp& app) {
  UiState& ui = app.GetUiState();
  ImGui::Begin("Errors");

  ImGui::SetNextItemWidth(140.0F);
  ImGui::InputInt("Source Filter", &ui.error_source_filter);
  ImGui::SetNextItemWidth(140.0F);
  ImGui::InputInt("Severity Filter", &ui.error_severity_filter);

  if (ImGui::BeginTable("errors_table", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupColumn("Time");
    ImGui::TableSetupColumn("Severity");
    ImGui::TableSetupColumn("Source");
    ImGui::TableSetupColumn("Address");
    ImGui::TableSetupColumn("Message");
    ImGui::TableHeadersRow();

    for (const ErrorEvent& event : app.GetErrorLog().Snapshot()) {
      if (ui.error_source_filter >= 0 && static_cast<int>(event.source) != ui.error_source_filter) {
        continue;
      }
      if (ui.error_severity_filter >= 0 && static_cast<int>(event.severity) != ui.error_severity_filter) {
        continue;
      }

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%" PRIu64, event.master_time);
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(SeverityLabel(event.severity));
      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(SourceLabel(event.source));
      ImGui::TableSetColumnIndex(3);
      if (event.address.has_value()) {
        if (ImGui::SmallButton("Jump")) {
          app.JumpToAddress(*event.address);
        }
        ImGui::SameLine();
        ImGui::Text("$%02X:%04X", static_cast<unsigned>(*event.address >> 16),
                    static_cast<unsigned>(*event.address & 0xFFFFU));
      } else {
        ImGui::TextUnformatted("-");
      }
      ImGui::TableSetColumnIndex(4);
      ImGui::TextWrapped("%s", event.message.c_str());
    }
    ImGui::EndTable();
  }

  ImGui::End();
}

}  // namespace pupsnes::debugger
