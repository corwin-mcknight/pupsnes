#include <cinttypes>
#include <cstdio>
#include <optional>
#include <string_view>

#include "debugger/app.h"
#include "imgui.h"
#include "panels.h"
#include "pupsnes/debugger/microop_trace.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/5a22/micro_op_strings.h"

namespace pupsnes::debugger {

namespace {

const char* StatusLabel(MicroOpStatus s) {
  switch (s) {
    case MicroOpStatus::kPending:
      return "Pending";
    case MicroOpStatus::kExecuted:
      return "Executed";
    case MicroOpStatus::kSkipped:
      return "Skipped";
  }
  return "?";
}

const char* StatusGlyph(MicroOpStatus s) {
  switch (s) {
    case MicroOpStatus::kExecuted:
      return "v";
    case MicroOpStatus::kSkipped:
      return "-";
    case MicroOpStatus::kPending:
      return "?";
  }
  return "?";
}

ImVec4 StatusColor(MicroOpStatus s) {
  switch (s) {
    case MicroOpStatus::kExecuted:
      return ImVec4(0.45F, 0.85F, 0.45F, 1.0F);
    case MicroOpStatus::kSkipped:
      return ImVec4(0.75F, 0.75F, 0.45F, 1.0F);
    case MicroOpStatus::kPending:
      return ImVec4(0.55F, 0.55F, 0.55F, 1.0F);
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
  if (!ImGui::Begin("Micro-op Trace")) {
    ImGui::End();
    return;
  }

  auto& trace = app.GetMicroOpTrace();
  static bool auto_scroll = true;

  if (ImGui::Button("Clear")) {
    trace.Clear();
  }
  ImGui::SameLine();
  ImGui::Checkbox("Auto-scroll", &auto_scroll);
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
  ImGui::Text("Retired");
  static uint64_t last_newest_seq = 0;
  const uint64_t newest_seq =
      trace.RetiredSize() > 0 ? trace.RetiredAt(trace.RetiredSize() - 1)->retired_seq : 0;
  const bool newest_changed = (newest_seq != last_newest_seq);
  last_newest_seq = newest_seq;
  if (ImGui::BeginChild("retired", ImVec2(0, 0), true)) {
    for (std::size_t i = 0; i < trace.RetiredSize(); ++i) {
      const auto* t = trace.RetiredAt(i);
      if (t == nullptr) {
        continue;
      }
      char hdr[96];
      std::snprintf(hdr, sizeof(hdr), "#%" PRIu64 "  $%06X  %02X  %.*s  (%u cycles)", t->retired_seq,
                    static_cast<unsigned>(t->opcode_address), static_cast<unsigned>(t->opcode),
                    static_cast<int>(t->mnemonic.size()), t->mnemonic.data(), static_cast<unsigned>(t->op_count));
      ImGui::PushID(static_cast<int>(i));
      const bool is_newest = (i + 1 == trace.RetiredSize());
      if (newest_changed) {
        ImGui::SetNextItemOpen(is_newest, ImGuiCond_Always);
      }
      if (ImGui::TreeNode(hdr)) {
        RenderTrace(*t, std::nullopt);
        ImGui::TreePop();
      }
      ImGui::PopID();
    }
    if (auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0F) {
      ImGui::SetScrollHereY(1.0F);
    }
  }
  ImGui::EndChild();

  ImGui::End();
}

}  // namespace pupsnes::debugger
