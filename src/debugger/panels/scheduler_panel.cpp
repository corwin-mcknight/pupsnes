#include <cinttypes>

#include "debugger/app.h"
#include "debugger/time_format.h"
#include "imgui.h"
#include "panels.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/signal_event.h"

namespace pupsnes::debugger {

namespace {

const char* SignalKindName(SignalKind k) {
  switch (k) {
    case SignalKind::kFrameEnd:          return "FrameEnd";
    case SignalKind::kVblankNmiBoundary: return "VblankNmiBoundary";
    case SignalKind::kHIrqMatch:         return "HIrqMatch";
    case SignalKind::kApuSampleDeadline: return "ApuSampleDeadline";
    case SignalKind::kDmaBurstComplete:  return "DmaBurstComplete";
    case SignalKind::kHdmaFire:          return "HdmaFire";
    default:                             return "?";
  }
}

}  // namespace

void RenderSchedulerPanel(DebuggerApp& app) {
  if (!app.GetUiState().show_scheduler_panel) return;
  if (!ImGui::Begin("Scheduler", &app.GetUiState().show_scheduler_panel)) {
    ImGui::End();
    return;
  }

  const TimeMasterT now = app.GetSnes().GetMasterTime();
  const auto snapshot = app.GetSnes().GetScheduler().SnapshotSignalQueue();

  ImGui::TextUnformatted("Master Time:");
  ImGui::SameLine();
  TextMasterTime(now, now, app.GetUiState().time_display_mode);
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  ImGui::Text("Pending: %zu", snapshot.size());
  if (!snapshot.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text("Next +%" PRId64,
                static_cast<int64_t>(snapshot.front().master_time) - static_cast<int64_t>(now));
  }
  ImGui::Separator();

  if (snapshot.empty()) {
    ImGui::TextDisabled("(queue empty)");
    ImGui::End();
    return;
  }

  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                                     ImGuiTableFlags_Resizable;
  if (ImGui::BeginTable("scheduler_table", 3, kFlags)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time");
    ImGui::TableSetupColumn("+Delta");
    ImGui::TableSetupColumn("Signal");
    ImGui::TableHeadersRow();

    for (const SignalEventView& event : snapshot) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      TextMasterTime(event.master_time, now, app.GetUiState().time_display_mode);

      ImGui::TableSetColumnIndex(1);
      const int64_t delta = static_cast<int64_t>(event.master_time) - static_cast<int64_t>(now);
      if (delta == 0) {
        ImGui::TextDisabled("now");
      } else {
        ImGui::Text("%+" PRId64, delta);
      }

      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(SignalKindName(event.kind));
    }
    ImGui::EndTable();
  }
  ImGui::End();
}

}  // namespace pupsnes::debugger
