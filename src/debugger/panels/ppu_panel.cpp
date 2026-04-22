#include "debugger/app.h"
#include "imgui.h"
#include "panels.h"

namespace pupsnes::debugger {

void RenderPpuPanel(DebuggerApp& app) {
  if (!app.GetUiState().show_ppu_panel) {
    return;
  }
  if (!ImGui::Begin("PPU", &app.GetUiState().show_ppu_panel)) {
    ImGui::End();
    return;
  }

  constexpr float kAspect = 256.0F / 224.0F;
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  float width = avail.x;
  float height = width / kAspect;
  if (height > avail.y) {
    height = avail.y;
    width = height * kAspect;
  }

  const ImVec2 cursor = ImGui::GetCursorScreenPos();
  const ImVec2 top_left(cursor.x + ((avail.x - width) * 0.5F), cursor.y);
  const ImVec2 bottom_right(top_left.x + width, top_left.y + height);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(top_left, bottom_right, IM_COL32(0, 0, 0, 255));

  ImGui::Dummy(ImVec2(width, height));
  ImGui::End();
}

}  // namespace pupsnes::debugger
