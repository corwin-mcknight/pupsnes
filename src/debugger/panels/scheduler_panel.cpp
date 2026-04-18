#include <cinttypes>

#include "debugger/app.h"
#include "imgui.h"
#include "panels.h"
#include "pupsnes/hw/scheduler.h"

namespace pupsnes::debugger {

void RenderSchedulerPanel(DebuggerApp& app) {
  ImGui::Begin("Scheduler");
  ImGui::Text("Master Time: %" PRIu64, app.GetSnes().GetMasterTime());
  ImGui::Separator();
  const auto snapshot = app.GetSnes().GetScheduler().SnapshotQueue();
  if (ImGui::BeginTable("scheduler_table", 4,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupColumn("Time");
    ImGui::TableSetupColumn("Device");
    ImGui::TableSetupColumn("Phase");
    ImGui::TableSetupColumn("Type");
    ImGui::TableHeadersRow();

    for (const SchedulerEventView& event : snapshot) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%" PRIu64, event.time);
      ImGui::TableSetColumnIndex(1);
      if (event.device_id.has_value()) {
        ImGui::Text("%u", static_cast<unsigned>(*event.device_id));
      } else {
        ImGui::TextUnformatted("-");
      }
      ImGui::TableSetColumnIndex(2);
      ImGui::Text("%d", static_cast<int>(event.subphase));
      ImGui::TableSetColumnIndex(3);
      ImGui::Text("%d", static_cast<int>(event.type));
    }
    ImGui::EndTable();
  }
  ImGui::End();
}

}  // namespace pupsnes::debugger
