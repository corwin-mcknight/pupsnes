#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

#include "debugger/app.h"
#include "debugger/ui_utils.h"
#include "imgui.h"
#include "panel_utils.h"
#include "panels.h"
#include "pupsnes/debugger/disasm.h"

namespace pupsnes::debugger {

void RenderDisasmPanel(DebuggerApp& app) {
  UiState& ui = app.GetUiState();
  ScopedPanel panel("Disassembly", ui.show_disasm_panel);
  if (!panel) return;
  if (!app.HasLoadedRom()) {
    ImGui::TextUnformatted("Load a ROM to inspect disassembly.");
    return;
  }

  SnesAddrT address = ui.follow_pc ? app.GetCurrentPc() : ui.disasm_address;
  ImGui::Checkbox("Follow PC", &ui.follow_pc);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(160.0F);
  ImGui::InputScalar("Start", ImGuiDataType_U32, &ui.disasm_address, nullptr, nullptr, "%06X",
                     ImGuiInputTextFlags_CharsHexadecimal);

  ImGui::Separator();

  constexpr float kGutterWidth = 60.0F;
  constexpr float kLaneWidth = 6.0F;
  constexpr int kMaxLanes = 9;
  constexpr ImU32 kBranchColor = IM_COL32(140, 200, 240, 220);
  constexpr ImU32 kBranchColorOff = IM_COL32(170, 170, 170, 180);

  struct LineRec {
    SnesAddrT pc;
    float y_center;
  };
  struct BranchRec {
    std::size_t src_idx = 0;
    std::size_t dst_idx = 0;  // only valid when dst_visible
    bool dst_visible = false;
    SnesAddrT target = 0;
    bool target_above = false;
    std::size_t lane = 0;
  };

  std::vector<LineRec> lines;
  std::vector<BranchRec> branches;
  std::unordered_map<SnesAddrT, std::size_t> pc_to_idx;
  lines.reserve(128);
  branches.reserve(32);

  const SnesAddrT current_pc = app.GetCurrentPc();
  const ImVec4 current_color(0.96F, 0.82F, 0.28F, 1.0F);
  const float bottom_y = ImGui::GetCursorPosY() + ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();
  float text_left_x = -1.0F;
  float panel_top_y = 0.0F;

  for (int line_index = 0; line_index < 4096; ++line_index) {
    if (ImGui::GetCursorPosY() >= bottom_y) {
      break;
    }
    const DisassembledInstruction line =
        DisassembleInstruction(app.GetSnes(), address, app.GetSnes().GetCpu().GetRegs().P);
    const bool is_current = line.pc == current_pc;

    const ImVec2 line_top_screen = ImGui::GetCursorScreenPos();
    const float line_height = ImGui::GetTextLineHeight();
    if (line_index == 0) {
      panel_top_y = line_top_screen.y;
    }

    pc_to_idx.emplace(line.pc, lines.size());
    lines.push_back({line.pc, line_top_screen.y + line_height * 0.5F});

    ImGui::PushID(line_index);
    if (ImGui::SmallButton(app.GetBreakpoints().IsEnabled(line.pc) ? "B" : ".")) {
      app.GetBreakpoints().Toggle(line.pc);
    }
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(kGutterWidth, 0.0F));
    ImGui::SameLine();
    if (text_left_x < 0.0F) {
      text_left_x = ImGui::GetCursorScreenPos().x;
    }

    const std::string addr = FormatAddress24(line.pc);
    if (is_current) {
      ImGui::TextColored(current_color, "%s  %s", addr.c_str(), line.text.c_str());
    } else {
      ImGui::Text("%s  %s", addr.c_str(), line.text.c_str());
    }

    if (is_current) {
      const char* arrow = "<--";
      const float arrow_w = ImGui::CalcTextSize(arrow).x;
      const float right = ImGui::GetWindowContentRegionMax().x;
      ImGui::SameLine();
      ImGui::SetCursorPosX(right - arrow_w);
      ImGui::TextColored(current_color, "%s", arrow);
    }
    ImGui::PopID();

    if (const std::optional<SnesAddrT> target = GetBranchTarget(line)) {
      BranchRec b;
      b.src_idx = lines.size() - 1U;
      b.target = *target;
      b.target_above = *target < line.pc;
      branches.push_back(b);
    }

    address = (address + line.length) & 0x00FFFFFFU;
  }

  if (branches.empty() || text_left_x < 0.0F || lines.empty()) {
    return;
  }

  const float panel_bot_y = lines.back().y_center + ImGui::GetTextLineHeight() * 0.5F;

  for (BranchRec& b : branches) {
    const auto it = pc_to_idx.find(b.target);
    if (it != pc_to_idx.end()) {
      b.dst_idx = it->second;
      b.dst_visible = true;
    }
  }

  const std::size_t last_idx = lines.size() - 1U;
  auto span_for = [&](const BranchRec& b) -> std::pair<std::size_t, std::size_t> {
    const std::size_t s = b.src_idx;
    const std::size_t e = b.dst_visible ? b.dst_idx : (b.target_above ? std::size_t{0} : last_idx);
    return {std::min(s, e), std::max(s, e)};
  };

  std::vector<std::size_t> order(branches.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    const auto sa = span_for(branches[a]);
    const auto sb = span_for(branches[b]);
    return (sa.second - sa.first) > (sb.second - sb.first);
  });

  constexpr std::size_t kMaxLanesSz = static_cast<std::size_t>(kMaxLanes);
  std::vector<std::vector<std::pair<std::size_t, std::size_t>>> lane_intervals;
  for (const std::size_t idx : order) {
    const auto span = span_for(branches[idx]);
    std::size_t lane = 0;
    while (true) {
      if (lane >= lane_intervals.size()) {
        lane_intervals.emplace_back();
      }
      bool conflict = false;
      for (const auto& [lo, hi] : lane_intervals[lane]) {
        if (!(span.second < lo || span.first > hi)) {
          conflict = true;
          break;
        }
      }
      if (!conflict) {
        lane_intervals[lane].emplace_back(span);
        branches[idx].lane = std::min(lane, kMaxLanesSz - 1U);
        break;
      }
      ++lane;
    }
  }

  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  const float right_edge = text_left_x - 4.0F;
  const float arrow_head_len = 5.0F;
  const float thickness = 1.5F;

  for (const BranchRec& b : branches) {
    const float lane_x = right_edge - kLaneWidth * static_cast<float>(b.lane + 1U);
    const float src_y = lines[b.src_idx].y_center;
    const ImU32 color = b.dst_visible ? kBranchColor : kBranchColorOff;

    draw_list->AddLine(ImVec2(lane_x, src_y), ImVec2(right_edge, src_y), color, thickness);

    if (b.dst_visible) {
      const float dst_y = lines[b.dst_idx].y_center;
      draw_list->AddLine(ImVec2(lane_x, src_y), ImVec2(lane_x, dst_y), color, thickness);
      draw_list->AddLine(ImVec2(lane_x, dst_y), ImVec2(right_edge - arrow_head_len, dst_y), color, thickness);
      const ImVec2 tip(right_edge, dst_y);
      draw_list->AddTriangleFilled(tip, ImVec2(tip.x - arrow_head_len, tip.y - 3.0F),
                                   ImVec2(tip.x - arrow_head_len, tip.y + 3.0F), color);
    } else {
      const float edge_y = b.target_above ? panel_top_y : panel_bot_y;
      draw_list->AddLine(ImVec2(lane_x, src_y), ImVec2(lane_x, edge_y), color, thickness);
      const float caret_dy = b.target_above ? 4.0F : -4.0F;
      draw_list->AddLine(ImVec2(lane_x - 3.0F, edge_y + caret_dy), ImVec2(lane_x, edge_y), color, thickness);
      draw_list->AddLine(ImVec2(lane_x + 3.0F, edge_y + caret_dy), ImVec2(lane_x, edge_y), color, thickness);
    }
  }
}

}  // namespace pupsnes::debugger
