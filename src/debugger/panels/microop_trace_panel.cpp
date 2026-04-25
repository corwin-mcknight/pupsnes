#include <cinttypes>
#include <cstdio>
#include <optional>
#include <string_view>

#include "debugger/app.h"
#include "imgui.h"
#include "panel_utils.h"
#include "panels.h"
#include "pupsnes/debugger/microop_trace.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/5a22/micro_op_strings.h"

namespace pupsnes::debugger {

namespace {

const char* StatusLabel(MicroOpStatus s) {
  switch (s) {
    case MicroOpStatus::kPending: return "Pending";
    case MicroOpStatus::kExecuted: return "Executed";
    case MicroOpStatus::kSkipped: return "Skipped";
  }
  return "?";
}

const char* StatusGlyph(MicroOpStatus s) {
  switch (s) {
    case MicroOpStatus::kExecuted: return "v";
    case MicroOpStatus::kSkipped: return "-";
    case MicroOpStatus::kPending: return "?";
  }
  return "?";
}

ImVec4 StatusColor(MicroOpStatus s) {
  switch (s) {
    case MicroOpStatus::kExecuted: return ImVec4(0.45F, 0.85F, 0.45F, 1.0F);
    case MicroOpStatus::kSkipped: return ImVec4(0.75F, 0.75F, 0.45F, 1.0F);
    case MicroOpStatus::kPending: return ImVec4(0.55F, 0.55F, 0.55F, 1.0F);
  }
  return ImVec4(1.0F, 1.0F, 1.0F, 1.0F);
}

void RenderRecordRow(const MicroOpRecord& rec) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::Text("%u", static_cast<unsigned>(rec.index));
  ImGui::TableSetColumnIndex(1);
  const std::string_view bus = ToString(rec.bus_action);
  const std::string_view internal = ToString(rec.internal_op);
  ImGui::Text("%.*s/%.*s", static_cast<int>(bus.size()), bus.data(), static_cast<int>(internal.size()),
              internal.data());
  ImGui::TableSetColumnIndex(2);
  ImGui::TextColored(StatusColor(rec.status), "%s", StatusGlyph(rec.status));
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", StatusLabel(rec.status));
  }
  ImGui::TableSetColumnIndex(3);
  ImGui::Text("$%02X", static_cast<unsigned>(rec.fetch_data));
  ImGui::TableSetColumnIndex(4);
  ImGui::Text("$%06X", static_cast<unsigned>(rec.addr));
  ImGui::TableSetColumnIndex(5);
  if (rec.has_bus) {
    ImGui::Text("$%06X=$%02X", static_cast<unsigned>(rec.bus_addr), static_cast<unsigned>(rec.bus_value));
  } else {
    ImGui::TextDisabled("-");
  }
}

void RenderTrace(const InstructionTrace& t, std::optional<uint8_t> highlight_index) {
  if (ImGui::BeginTable("microops", 6,
                        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                            ImGuiTableFlags_Resizable)) {
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("S", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("fetch", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("addr", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("bus", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (uint8_t i = 0; i < t.op_count; ++i) {
      const bool is_current = highlight_index.has_value() && *highlight_index == t.ops[i].index;
      if (is_current) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.95F, 0.4F, 1.0F));
      }
      RenderRecordRow(t.ops[i]);
      if (is_current) {
        ImGui::PopStyleColor();
      }
    }
    ImGui::EndTable();
  }
}

}  // namespace

void RenderMicroOpTracePanel(DebuggerApp& app) {
  ScopedPanel panel("Micro-op Trace", app.GetUiState().show_microop_trace_panel);
  if (!panel) return;

  auto& trace = app.GetMicroOpTrace();

  if (ImGui::Button("Clear")) {
    trace.Clear();
  }
  ImGui::SameLine();
  ImGui::Text("Retired: %zu", trace.RetiredSize());

  ImGui::Separator();
  ImGui::Text("Current instruction");
  if (trace.Current().has_value()) {
    const auto& cur = *trace.Current();
    ImGui::Text("$%06X  %02X  %.*s", static_cast<unsigned>(cur.opcode_address), static_cast<unsigned>(cur.opcode),
                static_cast<int>(cur.mnemonic.size()), cur.mnemonic.data());
    RenderTrace(cur, app.GetSnes().GetCpu().GetMicroOpIndex());
  } else {
    ImGui::TextDisabled("(between instructions)");
  }

  ImGui::Separator();
  ImGui::Text("Retired (last 2)");
  const std::size_t retired_size = trace.RetiredSize();
  const std::size_t shown = retired_size < 2 ? retired_size : 2;
  for (std::size_t k = 0; k < shown; ++k) {
    const std::size_t i = retired_size - shown + k;
    const auto* t = trace.RetiredAt(i);
    if (t == nullptr) {
      continue;
    }
    ImGui::PushID(static_cast<int>(i));
    ImGui::Text("#%" PRIu64 "  $%06X  %02X  %.*s  (%u cycles)", t->retired_seq,
                static_cast<unsigned>(t->opcode_address), static_cast<unsigned>(t->opcode),
                static_cast<int>(t->mnemonic.size()), t->mnemonic.data(), static_cast<unsigned>(t->op_count));
    RenderTrace(*t, std::nullopt);
    ImGui::PopID();
  }
}

}  // namespace pupsnes::debugger
