#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>

#include "debugger/app.h"
#include "debugger/time_format.h"
#include "imgui.h"
#include "panels.h"

namespace pupsnes::debugger {

namespace {

constexpr float kFadeSeconds = 1.0F;

// Lavender highlight (matches ImGuiCol_CheckMark palette in app.cpp).
constexpr uint8_t kHighlightR = 182;
constexpr uint8_t kHighlightG = 177;
constexpr uint8_t kHighlightB = 226;

void Decay(float& h, float dt) { h = std::max(0.0F, h - dt / kFadeSeconds); }

ImU32 HighlightColor(float intensity) {
  const auto alpha = static_cast<uint8_t>(std::clamp(intensity, 0.0F, 1.0F) * 180.0F);
  return IM_COL32(kHighlightR, kHighlightG, kHighlightB, alpha);
}

void HighlightValueCell(float intensity) {
  if (intensity <= 0.0F) {
    return;
  }
  ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, HighlightColor(intensity));
}

void RenderValueCell(const char* label, const char* value, float intensity) {
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(label);
  ImGui::TableNextColumn();
  HighlightValueCell(intensity);
  ImGui::TextUnformatted(value);
}

void RenderFlagLetter(const char* letter, bool set, float intensity) {
  const ImVec2 text_size = ImGui::CalcTextSize(letter);
  const ImVec2 cursor = ImGui::GetCursorScreenPos();
  if (intensity > 0.0F) {
    const float pad_x = 2.0F;
    const float pad_y = 1.0F;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cursor.x - pad_x, cursor.y - pad_y),
                                              ImVec2(cursor.x + text_size.x + pad_x, cursor.y + text_size.y + pad_y),
                                              HighlightColor(intensity), 2.0F);
  }
  if (set) {
    ImGui::TextUnformatted(letter);
  } else {
    ImGui::TextDisabled("%s", letter);
  }
}

const char* ModeString(const CpuFlags& p) {
  if (p.E) {
    return "emulation (8/8)";
  }
  if (p.M && p.X) {
    return "native 8/8";
  }
  if (p.M && !p.X) {
    return "native 8/16";
  }
  if (!p.M && p.X) {
    return "native 16/8";
  }
  return "native 16/16";
}

}  // namespace

void RenderRegistersPanel(DebuggerApp& app) {
  if (!app.GetUiState().show_registers_panel) {
    return;
  }
  if (!ImGui::Begin("Registers", &app.GetUiState().show_registers_panel)) {
    ImGui::End();
    return;
  }
  if (!app.HasLoadedRom()) {
    ImGui::TextUnformatted("Load a ROM to inspect CPU state.");
    ImGui::End();
    return;
  }

  const CPU& cpu = app.GetSnes().GetCpu();
  const CPU::Regs regs = cpu.GetRegs();
  const uint64_t retired = cpu.GetRetiredInstructionCount();
  const bool a16 = !regs.P.E && !regs.P.M;
  const bool x16 = !regs.P.E && !regs.P.X;

  RegisterHistory& hist = app.GetUiState().register_history;
  const float dt = ImGui::GetIO().DeltaTime;

  auto bump = [](float& h, bool changed) {
    if (changed) {
      h = 1.0F;
    }
  };

  if (hist.valid && retired != hist.retired) {
    bump(hist.hi_A, regs.A != hist.A);
    bump(hist.hi_X, regs.X != hist.X);
    bump(hist.hi_Y, regs.Y != hist.Y);
    bump(hist.hi_SP, regs.SP != hist.SP);
    bump(hist.hi_DP, regs.DP != hist.DP);
    bump(hist.hi_PC, regs.PC != hist.PC);
    bump(hist.hi_PBR, regs.PBR != hist.PBR);
    bump(hist.hi_DBR, regs.DBR != hist.DBR);
    bump(hist.hi_N, regs.P.N != hist.N);
    bump(hist.hi_V, regs.P.V != hist.V);
    bump(hist.hi_M, regs.P.M != hist.M);
    bump(hist.hi_Xf, regs.P.X != hist.Xf);
    bump(hist.hi_D, regs.P.D != hist.D);
    bump(hist.hi_I, regs.P.I != hist.I);
    bump(hist.hi_Z, regs.P.Z != hist.Z);
    bump(hist.hi_C, regs.P.C != hist.C);
    bump(hist.hi_E, regs.P.E != hist.E);
  }

  Decay(hist.hi_A, dt);
  Decay(hist.hi_X, dt);
  Decay(hist.hi_Y, dt);
  Decay(hist.hi_SP, dt);
  Decay(hist.hi_DP, dt);
  Decay(hist.hi_PC, dt);
  Decay(hist.hi_PBR, dt);
  Decay(hist.hi_DBR, dt);
  Decay(hist.hi_N, dt);
  Decay(hist.hi_V, dt);
  Decay(hist.hi_M, dt);
  Decay(hist.hi_Xf, dt);
  Decay(hist.hi_D, dt);
  Decay(hist.hi_I, dt);
  Decay(hist.hi_Z, dt);
  Decay(hist.hi_C, dt);
  Decay(hist.hi_E, dt);

  hist.A = regs.A;
  hist.X = regs.X;
  hist.Y = regs.Y;
  hist.SP = regs.SP;
  hist.DP = regs.DP;
  hist.PC = regs.PC;
  hist.PBR = regs.PBR;
  hist.DBR = regs.DBR;
  hist.N = regs.P.N;
  hist.V = regs.P.V;
  hist.M = regs.P.M;
  hist.Xf = regs.P.X;
  hist.D = regs.P.D;
  hist.I = regs.P.I;
  hist.Z = regs.P.Z;
  hist.C = regs.P.C;
  hist.E = regs.P.E;
  hist.retired = retired;
  hist.valid = true;

  char buf[32];

  if (ImGui::BeginTable("regs", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadInnerX)) {
    ImGui::TableNextRow();
    std::snprintf(buf, sizeof(buf), "$%02X:%04X", regs.PBR, regs.PC);
    RenderValueCell("PC", buf, std::max(hist.hi_PC, hist.hi_PBR));
    std::snprintf(buf, sizeof(buf), "$%02X", regs.DBR);
    RenderValueCell("DBR", buf, hist.hi_DBR);

    ImGui::TableNextRow();
    if (a16) {
      std::snprintf(buf, sizeof(buf), "$%04X", regs.A);
    } else {
      std::snprintf(buf, sizeof(buf), "$%02X (B:%02X)", regs.A & 0xFFU, (regs.A >> 8) & 0xFFU);
    }
    RenderValueCell("A", buf, hist.hi_A);
    std::snprintf(buf, sizeof(buf), "$%04X", regs.DP);
    RenderValueCell("DP", buf, hist.hi_DP);

    ImGui::TableNextRow();
    if (x16) {
      std::snprintf(buf, sizeof(buf), "$%04X", regs.X);
    } else {
      std::snprintf(buf, sizeof(buf), "$%02X", regs.X & 0xFFU);
    }
    RenderValueCell("X", buf, hist.hi_X);
    std::snprintf(buf, sizeof(buf), "$%04X", regs.SP);
    RenderValueCell("SP", buf, hist.hi_SP);

    ImGui::TableNextRow();
    if (x16) {
      std::snprintf(buf, sizeof(buf), "$%04X", regs.Y);
    } else {
      std::snprintf(buf, sizeof(buf), "$%02X", regs.Y & 0xFFU);
    }
    RenderValueCell("Y", buf, hist.hi_Y);
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();

    ImGui::EndTable();
  }

  ImGui::Separator();

  struct FlagEntry {
    const char* letter;
    bool set;
    float intensity;
  };
  const FlagEntry flags[] = {
      {"N", regs.P.N, hist.hi_N},  {"V", regs.P.V, hist.hi_V}, {"M", regs.P.M, hist.hi_M},
      {"X", regs.P.X, hist.hi_Xf}, {"D", regs.P.D, hist.hi_D}, {"I", regs.P.I, hist.hi_I},
      {"Z", regs.P.Z, hist.hi_Z},  {"C", regs.P.C, hist.hi_C}, {"E", regs.P.E, hist.hi_E},
  };
  for (size_t i = 0; i < std::size(flags); ++i) {
    if (i > 0) {
      ImGui::SameLine();
    }
    RenderFlagLetter(flags[i].letter, flags[i].set, flags[i].intensity);
  }

  ImGui::Separator();
  ImGui::Text("Mode   %s", ModeString(regs.P));
  ImGui::Text("Instr  %" PRIu64, static_cast<uint64_t>(retired));
  ImGui::Text("uOp    %u", static_cast<unsigned>(cpu.GetMicroOpIndex()));
  ImGui::TextUnformatted("MCyc  ");
  ImGui::SameLine();
  const TimeMasterT master_now = app.GetSnes().GetMasterTime();
  TextMasterTime(master_now, master_now, app.GetUiState().time_display_mode);
  ImGui::Text("DRAM   %" PRIu64 " win / %" PRIu64 " cyc  next@%" PRIu64, cpu.GetRefreshStallWindows(),
              cpu.GetRefreshStallCycles(), static_cast<uint64_t>(cpu.GetNextRefreshTime()));

  if (const auto& fault = cpu.GetFault(); fault.has_value()) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.95F, 0.25F, 0.25F, 1.0F), "Fault opcode $%02X at $%02X:%04X", fault->opcode,
                       static_cast<unsigned>(fault->opcode_address >> 16),
                       static_cast<unsigned>(fault->opcode_address & 0xFFFFU));
  }

  ImGui::End();
}

}  // namespace pupsnes::debugger
