#include "debugger/app.h"
#include "imgui.h"
#include "panels.h"

namespace pupsnes::debugger {

namespace {

void RenderFlag(bool value, const char* label) {
  ImGui::Text("%s:%d", label, value ? 1 : 0);
  ImGui::SameLine();
}

}  // namespace

void RenderRegistersPanel(DebuggerApp& app) {
  ImGui::Begin("Registers");
  if (!app.HasLoadedRom()) {
    ImGui::TextUnformatted("Load a ROM to inspect CPU state.");
    ImGui::End();
    return;
  }

  const CPU::Regs regs = app.GetSnes().GetCpu().GetRegs();
  ImGui::Text("A   $%04X", regs.A);
  ImGui::Text("X   $%04X", regs.X);
  ImGui::Text("Y   $%04X", regs.Y);
  ImGui::Text("SP  $%04X", regs.SP);
  ImGui::Text("DP  $%04X", regs.DP);
  ImGui::Text("PC  $%02X:%04X", regs.PBR, regs.PC);
  ImGui::Text("DBR $%02X", regs.DBR);
  ImGui::Text("Micro-op %u", static_cast<unsigned>(app.GetSnes().GetCpu().GetMicroOpIndex()));

  ImGui::Separator();
  RenderFlag(regs.P.N, "N");
  RenderFlag(regs.P.V, "V");
  RenderFlag(regs.P.M, "M");
  RenderFlag(regs.P.X, "X");
  RenderFlag(regs.P.D, "D");
  RenderFlag(regs.P.I, "I");
  RenderFlag(regs.P.Z, "Z");
  RenderFlag(regs.P.C, "C");
  RenderFlag(regs.P.E, "E");
  ImGui::NewLine();

  if (const auto& fault = app.GetSnes().GetCpu().GetFault(); fault.has_value()) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.95F, 0.25F, 0.25F, 1.0F), "Fault opcode $%02X at $%02X:%04X", fault->opcode,
                       static_cast<unsigned>(fault->opcode_address >> 16),
                       static_cast<unsigned>(fault->opcode_address & 0xFFFFU));
  }

  ImGui::End();
}

}  // namespace pupsnes::debugger
