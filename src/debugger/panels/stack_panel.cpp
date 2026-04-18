#include <cstdint>

#include "debugger/app.h"
#include "imgui.h"
#include "panels.h"

namespace pupsnes::debugger {

void RenderStackPanel(DebuggerApp& app) {
  ImGui::Begin("Stack");
  if (!app.HasLoadedRom()) {
    ImGui::TextUnformatted("Load a ROM to inspect the stack.");
    ImGui::End();
    return;
  }

  const CPU::Regs regs = app.GetSnes().GetCpu().GetRegs();
  const uint16_t sp = regs.SP;
  ImGui::Text("SP $%04X   E %u", sp, regs.P.E ? 1U : 0U);
  ImGui::Separator();

  constexpr int kRowsAbove = 4;
  constexpr int kRowsBelow = 24;

  if (ImGui::BeginTable("stack_table", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Byte", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    const int start = -kRowsAbove;
    const int end = kRowsBelow;
    for (int offset = start; offset <= end; ++offset) {
      const uint16_t target_sp = static_cast<uint16_t>(sp + offset);
      const SnesAddrT addr = static_cast<SnesAddrT>(target_sp);
      const DebugReadResult cell = app.GetSnes().GetSystemBus().DebugRead(addr);

      ImGui::TableNextRow();
      if (offset == 1) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(60, 90, 40, 160));
      } else if (offset == 0) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(90, 60, 40, 120));
      }

      ImGui::TableSetColumnIndex(0);
      ImGui::Text("$00:%04X", static_cast<unsigned>(target_sp));
      ImGui::TableSetColumnIndex(1);
      if (cell.ok) {
        ImGui::Text("%02X", static_cast<unsigned>(cell.value));
      } else {
        ImGui::TextUnformatted("??");
      }
      ImGui::TableSetColumnIndex(2);
      if (offset == 0) {
        ImGui::TextUnformatted("<- SP (next push)");
      } else if (offset == 1) {
        ImGui::TextUnformatted("top of stack");
      }
    }
    ImGui::EndTable();
  }

  ImGui::End();
}

}  // namespace pupsnes::debugger
