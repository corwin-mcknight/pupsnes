#include "debugger/app.h"
#include "imgui.h"
#include "panels.h"
#include "pupsnes/debugger/disasm.h"

namespace pupsnes::debugger {

void RenderDisasmPanel(DebuggerApp& app) {
  UiState& ui = app.GetUiState();
  ImGui::Begin("Disassembly");
  if (!app.HasLoadedRom()) {
    ImGui::TextUnformatted("Load a ROM to inspect disassembly.");
    ImGui::End();
    return;
  }

  SnesAddrT address = ui.follow_pc ? app.GetCurrentPc() : ui.disasm_address;
  ImGui::Checkbox("Follow PC", &ui.follow_pc);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(160.0F);
  ImGui::InputScalar("Start", ImGuiDataType_U32, &ui.disasm_address, nullptr, nullptr, "%06X",
                     ImGuiInputTextFlags_CharsHexadecimal);

  ImGui::Separator();
  const SnesAddrT current_pc = app.GetCurrentPc();
  for (int line_index = 0; line_index < 48; ++line_index) {
    const DisassembledInstruction line =
        DisassembleInstruction(app.GetSnes(), address, app.GetSnes().GetCpu().GetRegs().P);
    const bool is_current = line.pc == current_pc;

    ImGui::PushID(line_index);
    if (ImGui::SmallButton(app.GetBreakpoints().IsEnabled(line.pc) ? "B" : ".")) {
      app.GetBreakpoints().Toggle(line.pc);
    }
    ImGui::SameLine();

    if (is_current) {
      ImGui::TextColored(ImVec4(0.96F, 0.82F, 0.28F, 1.0F), "$%02X:%04X  %s", static_cast<unsigned>(line.pc >> 16),
                         static_cast<unsigned>(line.pc & 0xFFFFU), line.text.c_str());
    } else {
      ImGui::Text("$%02X:%04X  %s", static_cast<unsigned>(line.pc >> 16), static_cast<unsigned>(line.pc & 0xFFFFU),
                  line.text.c_str());
    }
    ImGui::PopID();

    address = (address + line.length) & 0x00FFFFFFU;
  }

  ImGui::End();
}

}  // namespace pupsnes::debugger
