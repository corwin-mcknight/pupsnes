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

// Renders a single button that toggles its Joypad state on click. While the
// button is held down (mouse pressed on it) the pad bit is also pressed —
// that lets the user execute a "press and release" gesture by click-and-drag
// off the button without needing a separate release click for fast inputs.
// Releasing the mouse without moving toggles back to the pre-click state.
void RenderPadButton(Joypad& joypad, Joypad::Button button, const char* label, ImVec2 size) {
  const bool latched = joypad.GetButton(button);
  if (latched) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.537F, 0.408F, 0.722F, 1.0F));  // purple accent
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65F, 0.50F, 0.82F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75F, 0.62F, 0.92F, 1.0F));
  }
  if (ImGui::Button(label, size)) {
    joypad.SetButton(button, !latched);
  }
  if (latched) {
    ImGui::PopStyleColor(3);
  }
}

}  // namespace

void RenderControllerPanel(DebuggerApp& app) {
  ScopedPanel panel("P1 Controller", app.GetUiState().show_controller_panel);
  if (!panel) return;

  Joypad& joypad = app.GetSnes().GetJoypad();

  // Shoulder row: L far-left, R far-right.
  RenderPadButton(joypad, Joypad::Button::kL, "L", kShoulderBtnSize);
  {
    const float row_width = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(row_width - kShoulderBtnSize.x);
  }
  RenderPadButton(joypad, Joypad::Button::kR, "R", kShoulderBtnSize);

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
    RenderPadButton(joypad, Joypad::Button::kUp, "^", kDPadBtnSize);

    RenderPadButton(joypad, Joypad::Button::kLeft, "<", kDPadBtnSize);
    ImGui::SameLine();
    ImGui::Dummy(kDPadBtnSize);
    ImGui::SameLine();
    RenderPadButton(joypad, Joypad::Button::kRight, ">", kDPadBtnSize);

    ImGui::Dummy(kDPadBtnSize);
    ImGui::SameLine();
    RenderPadButton(joypad, Joypad::Button::kDown, "v", kDPadBtnSize);
  }
  ImGui::EndGroup();

  ImGui::SameLine(0.0F, 24.0F);

  ImGui::BeginGroup();
  {
    ImGui::Dummy(ImVec2(0.0F, kDPadBtnSize.y * 0.5F));
    RenderPadButton(joypad, Joypad::Button::kSelect, "Select", kCenterBtnSize);
    ImGui::SameLine();
    RenderPadButton(joypad, Joypad::Button::kStart, "Start", kCenterBtnSize);
  }
  ImGui::EndGroup();

  ImGui::SameLine(0.0F, 24.0F);

  ImGui::BeginGroup();
  {
    // Action-button diamond: X on top, Y left, A right, B bottom. Matches the
    // SNES face layout so muscle memory works when clicking.
    ImGui::Dummy(kFaceBtnSize);
    ImGui::SameLine();
    RenderPadButton(joypad, Joypad::Button::kX, "X", kFaceBtnSize);

    RenderPadButton(joypad, Joypad::Button::kY, "Y", kFaceBtnSize);
    ImGui::SameLine();
    ImGui::Dummy(kFaceBtnSize);
    ImGui::SameLine();
    RenderPadButton(joypad, Joypad::Button::kA, "A", kFaceBtnSize);

    ImGui::Dummy(kFaceBtnSize);
    ImGui::SameLine();
    RenderPadButton(joypad, Joypad::Button::kB, "B", kFaceBtnSize);
  }
  ImGui::EndGroup();

  ImGui::Dummy(ImVec2(0.0F, 8.0F));
  if (ImGui::Button("Release All", ImVec2(0.0F, 0.0F))) {
    joypad.ReleaseAll();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("state: $%04X", static_cast<unsigned>(joypad.GetP1State()));

  // Keyboard binding hint. Must stay in sync with kP1KeyMap in app.cpp.
  ImGui::Dummy(ImVec2(0.0F, 4.0F));
  ImGui::TextDisabled("Keyboard:");
  ImGui::TextDisabled("  D-pad: arrows   B: Z   A: X   Y: A   X: S");
  ImGui::TextDisabled("  L: Q   R: W   Start: Enter   Select: Right Shift");
}

}  // namespace pupsnes::debugger
