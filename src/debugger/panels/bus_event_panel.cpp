#include <cinttypes>
#include <cstdint>

#include "debugger/app.h"
#include "imgui.h"
#include "panel_utils.h"
#include "panels.h"
#include "pupsnes/debugger/bus_event_log.h"

namespace pupsnes::debugger {

namespace {

struct KindInfo {
  const char* label;
  ImVec4 color;
};

KindInfo KindDisplay(BusEventKind kind) {
  switch (kind) {
    case BusEventKind::kFastRead: return {"FST R", ImVec4(0.60F, 0.85F, 0.60F, 1.0F)};
    case BusEventKind::kFastWrite: return {"FST W", ImVec4(0.85F, 0.70F, 0.50F, 1.0F)};
    case BusEventKind::kInlineRead: return {"INL R", ImVec4(0.55F, 0.80F, 0.95F, 1.0F)};
    case BusEventKind::kInlineWrite: return {"INL W", ImVec4(0.95F, 0.80F, 0.55F, 1.0F)};
    case BusEventKind::kScheduledRead: return {"SCH R", ImVec4(0.70F, 0.70F, 1.0F, 1.0F)};
    case BusEventKind::kScheduledWrite: return {"SCH W", ImVec4(1.0F, 0.75F, 0.75F, 1.0F)};
    case BusEventKind::kRejectedRead: return {"REJ R", ImVec4(0.85F, 0.45F, 0.45F, 1.0F)};
    case BusEventKind::kRejectedWrite: return {"REJ W", ImVec4(0.95F, 0.35F, 0.35F, 1.0F)};
  }
  return {"?", ImVec4(1.0F, 1.0F, 1.0F, 1.0F)};
}

}  // namespace

void RenderBusEventPanel(DebuggerApp& app) {
  ImGui::Begin("Bus");
  BusEventLog& log = app.GetBusEventLog();
  if (ImGui::SmallButton("Clear")) {
    log.Clear();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%zu / %zu", log.Size(), log.Capacity());

  const auto snapshot = log.Snapshot();
  if (ImGui::BeginTable(
          "bus_events", 4,
          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Data", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(snapshot.size()));
    while (clipper.Step()) {
      for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
        const BusEvent& e = snapshot[static_cast<std::size_t>(row)];
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%" PRIu64, static_cast<uint64_t>(e.master_time));

        ImGui::TableSetColumnIndex(1);
        const KindInfo info = KindDisplay(e.kind);
        ImGui::TextColored(info.color, "%s", info.label);

        ImGui::TableSetColumnIndex(2);
        TextAddress24(e.address);

        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%02X", e.data);
      }
    }

    ImGui::SetScrollHereY(1.0F);
    ImGui::EndTable();
  }

  ImGui::End();
}

}  // namespace pupsnes::debugger
