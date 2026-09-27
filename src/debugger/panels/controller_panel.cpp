#include <cstdint>

#include "debugger/app.h"
#include "imgui.h"
#include "panel_utils.h"
#include "panels.h"
#include "pupsnes/hw/input/joypad.h"

namespace pupsnes::debugger {

namespace {

constexpr ImVec2 kFaceBtnSize{32.0F, 32.0F};
constexpr ImVec2 kDPadBtnSize{28.0F, 28.0F};
constexpr ImVec2 kCenterBtnSize{52.0F, 22.0F};
constexpr ImVec2 kShoulderBtnSize{40.0F, 22.0F};

// A completed click toggles the latched Joypad state. Mouse-down and dragging
// away without clicking do not change the pad state.
void RenderPadButton(Joypad& joypad, unsigned port, Joypad::Button button, const char* label, ImVec2 size) {
  const bool latched = joypad.GetButton(button, port);
  if (latched) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.537F, 0.408F, 0.722F, 1.0F));  // purple accent
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65F, 0.50F, 0.82F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75F, 0.62F, 0.92F, 1.0F));
  }
  if (ImGui::Button(label, size)) {
    joypad.SetButton(button, !latched, port);
  }
  if (latched) {
    ImGui::PopStyleColor(3);
  }
}

}  // namespace

void RenderController(DebuggerApp& app, unsigned port, bool& visible) {
  ScopedPanel panel(port == 0 ? "P1 Controller" : "P2 Controller", visible);
  if (!panel) return;

  Joypad& joypad = app.GetSnes().GetJoypad();

  // Shoulder row: L far-left, R far-right.
  RenderPadButton(joypad, port, Joypad::Button::kL, "L", kShoulderBtnSize);
  {
    const float row_width = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(row_width - kShoulderBtnSize.x);
  }
  RenderPadButton(joypad, port, Joypad::Button::kR, "R", kShoulderBtnSize);

  ImGui::Dummy(ImVec2(0.0F, 6.0F));

  // Body row: D-pad on the left, Select+Start in the middle, A/B/X/Y diamond
  // on the right. Each cluster is built as its own ImGui group so the
  // horizontal alignment between them is independent of the cluster's
  // internal multi-row layout.
  ImGui::BeginGroup();
  {
    // D-pad: 3x3 with corners empty.
    ImGui::Dummy(kDPadBtnSize);
    ImGui::SameLine();
    RenderPadButton(joypad, port, Joypad::Button::kUp, "^", kDPadBtnSize);

    RenderPadButton(joypad, port, Joypad::Button::kLeft, "<", kDPadBtnSize);
    ImGui::SameLine();
    ImGui::Dummy(kDPadBtnSize);
    ImGui::SameLine();
    RenderPadButton(joypad, port, Joypad::Button::kRight, ">", kDPadBtnSize);

    ImGui::Dummy(kDPadBtnSize);
    ImGui::SameLine();
    RenderPadButton(joypad, port, Joypad::Button::kDown, "v", kDPadBtnSize);
  }
  ImGui::EndGroup();

  ImGui::SameLine(0.0F, 24.0F);

  ImGui::BeginGroup();
  {
    ImGui::Dummy(ImVec2(0.0F, kDPadBtnSize.y * 0.5F));
    RenderPadButton(joypad, port, Joypad::Button::kSelect, "Select", kCenterBtnSize);
    ImGui::SameLine();
    RenderPadButton(joypad, port, Joypad::Button::kStart, "Start", kCenterBtnSize);
  }
  ImGui::EndGroup();

  ImGui::SameLine(0.0F, 24.0F);

  ImGui::BeginGroup();
  {
    // Action-button diamond: X on top, Y left, A right, B bottom. Matches the
    // SNES face layout so muscle memory works when clicking.
    ImGui::Dummy(kFaceBtnSize);
    ImGui::SameLine();
    RenderPadButton(joypad, port, Joypad::Button::kX, "X", kFaceBtnSize);

    RenderPadButton(joypad, port, Joypad::Button::kY, "Y", kFaceBtnSize);
    ImGui::SameLine();
    ImGui::Dummy(kFaceBtnSize);
    ImGui::SameLine();
    RenderPadButton(joypad, port, Joypad::Button::kA, "A", kFaceBtnSize);

    ImGui::Dummy(kFaceBtnSize);
    ImGui::SameLine();
    RenderPadButton(joypad, port, Joypad::Button::kB, "B", kFaceBtnSize);
  }
  ImGui::EndGroup();

  ImGui::Dummy(ImVec2(0.0F, 8.0F));
  if (ImGui::Button("Release All", ImVec2(0.0F, 0.0F))) {
    joypad.ReleaseAll(port);
  }
  ImGui::SameLine();
  ImGui::TextDisabled("state: $%04X", static_cast<unsigned>(port == 0 ? joypad.GetP1State() : joypad.GetP2State()));

  // Keyboard binding hints must stay in sync with the maps in app.cpp.
  ImGui::Dummy(ImVec2(0.0F, 4.0F));
  ImGui::TextDisabled("Keyboard:");
  if (port == 0) {
    ImGui::TextDisabled("  D-pad: arrows   B: Z   A: X   Y: A   X: S");
    ImGui::TextDisabled("  L: Q   R: W   Start: Enter   Select: Right Shift");
  } else {
    ImGui::TextDisabled("  D-pad: IJKL   B: N   A: M   Y: V   X: C");
    ImGui::TextDisabled("  L: U   R: O   Start: H   Select: G");
  }
}

void RenderControllerPanel(DebuggerApp& app) {
  RenderController(app, 0, app.GetUiState().show_controller_panel);
  RenderController(app, 1, app.GetUiState().show_p2_controller_panel);
}

}  // namespace pupsnes::debugger
