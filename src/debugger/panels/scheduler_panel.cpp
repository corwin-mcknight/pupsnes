#include <cinttypes>

#include "debugger/app.h"
#include "imgui.h"
#include "panels.h"
#include "pupsnes/hw/scheduler.h"

namespace pupsnes::debugger {

namespace {

const char* DeviceName(DeviceIdT id) {
  // Registration order in SNES constructor: CPU, Cartridge, WRAM.
  // Scheduler and SystemBus are not Devices.
  switch (id) {
    case 0:
      return "CPU";
    case 1:
      return "Cartridge";
    case 2:
      return "WRAM";
    default:
      return nullptr;
  }
}

const char* PhaseName(SchedulerPhase phase) {
  switch (phase) {
    case SchedulerPhase::kCommitComplete:
      return "CommitComplete";
    case SchedulerPhase::kWakeSample:
      return "WakeSample";
    case SchedulerPhase::kRun:
      return "Run";
  }
  return "?";
}

const char* TypeName(EventType type) {
  switch (type) {
    case EventType::kDeviceRun:
      return "DeviceRun";
    case EventType::kDeviceBoundary:
      return "Boundary";
  }
  return "?";
}

ImU32 TypeColor(EventType type) {
  switch (type) {
    case EventType::kDeviceRun:
      return IM_COL32(120, 180, 255, 255);
    case EventType::kDeviceBoundary:
      return IM_COL32(255, 200, 120, 255);
  }
  return IM_COL32(200, 200, 200, 255);
}

}  // namespace

void RenderSchedulerPanel(DebuggerApp& app) {
  ImGui::Begin("Scheduler");

  const TimeMasterT now = app.GetSnes().GetMasterTime();
  const auto snapshot = app.GetSnes().GetScheduler().SnapshotQueue();

  ImGui::Text("Master Time: %" PRIu64, now);
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  ImGui::Text("Pending: %zu", snapshot.size());
  if (!snapshot.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    const TimeMasterT head = snapshot.front().time;
    const int64_t delta = static_cast<int64_t>(head) - static_cast<int64_t>(now);
    ImGui::Text("Next +%" PRId64, delta);
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
  if (ImGui::BeginTable("scheduler_table", 5, kFlags)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time");
    ImGui::TableSetupColumn("+Delta");
    ImGui::TableSetupColumn("Device");
    ImGui::TableSetupColumn("Phase");
    ImGui::TableSetupColumn("Type");
    ImGui::TableHeadersRow();

    for (const SchedulerEventView& event : snapshot) {
      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%" PRIu64, event.time);

      ImGui::TableSetColumnIndex(1);
      const int64_t delta = static_cast<int64_t>(event.time) - static_cast<int64_t>(now);
      if (delta == 0) {
        ImGui::TextDisabled("now");
      } else {
        ImGui::Text("%+" PRId64, delta);
      }

      ImGui::TableSetColumnIndex(2);
      if (event.device_id.has_value()) {
        const DeviceIdT id = *event.device_id;
        const char* name = DeviceName(id);
        if (name != nullptr) {
          ImGui::Text("%s (#%u)", name, static_cast<unsigned>(id));
        } else {
          ImGui::Text("#%u", static_cast<unsigned>(id));
        }
      } else {
        ImGui::TextDisabled("-");
      }

      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(PhaseName(event.subphase));

      ImGui::TableSetColumnIndex(4);
      ImGui::PushStyleColor(ImGuiCol_Text, TypeColor(event.type));
      ImGui::TextUnformatted(TypeName(event.type));
      ImGui::PopStyleColor();
    }
    ImGui::EndTable();
  }
  ImGui::End();
}

}  // namespace pupsnes::debugger
