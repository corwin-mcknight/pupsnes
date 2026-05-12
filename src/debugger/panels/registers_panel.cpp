#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>

#include "debugger/app.h"
#include "debugger/time_format.h"
#include "imgui.h"
#include "panel_utils.h"
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
  ScopedPanel panel("Registers", app.GetUiState().show_registers_panel);
  if (!panel) return;
  if (!app.HasLoadedRom()) {
    ImGui::TextUnformatted("Load a ROM to inspect CPU state.");
    return;
  }

  const CPU& cpu = app.GetSnes().GetCpu();
  const CPU::Regs regs = cpu.GetRegs();
  const uint64_t retired = cpu.GetRetiredInstructionCount();
  const bool a16 = !regs.P.E && !regs.P.M;
  const bool x16 = !regs.P.E && !regs.P.X;

  RegisterHistory& hist = app.GetUiState().register_history;
  const float dt = ImGui::GetIO().DeltaTime;
  const bool retired_changed = hist.valid && retired != hist.retired;

  auto track = [&](float& hi, auto& prev, auto current) {
    if (retired_changed && current != prev) hi = 1.0F;
    Decay(hi, dt);
    prev = current;
  };

  track(hist.hi_A, hist.A, regs.A);
  track(hist.hi_X, hist.X, regs.X);
  track(hist.hi_Y, hist.Y, regs.Y);
  track(hist.hi_SP, hist.SP, regs.SP);
  track(hist.hi_DP, hist.DP, regs.DP);
  track(hist.hi_PC, hist.PC, regs.PC);
  track(hist.hi_PBR, hist.PBR, regs.PBR);
  track(hist.hi_DBR, hist.DBR, regs.DBR);
  track(hist.hi_N, hist.N, regs.P.N);
  track(hist.hi_V, hist.V, regs.P.V);
  track(hist.hi_M, hist.M, regs.P.M);
  track(hist.hi_Xf, hist.Xf, regs.P.X);
  track(hist.hi_D, hist.D, regs.P.D);
  track(hist.hi_I, hist.I, regs.P.I);
  track(hist.hi_Z, hist.Z, regs.P.Z);
  track(hist.hi_C, hist.C, regs.P.C);
  track(hist.hi_E, hist.E, regs.P.E);
  hist.retired = retired;
  hist.valid = true;

  char buf[32];

  if (ImGui::BeginTable("regs", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadInnerX)) {
    // Lock columns to the widest possible content so values don't reflow as
    // glyph widths or A's 8-bit "(B:XX)" suffix come and go.
    const float label1_w = ImGui::CalcTextSize("PC").x;
    const float label2_w = ImGui::CalcTextSize("DBR").x;
    const float value1_w = ImGui::CalcTextSize("$BB (B:BB)").x;
    const float value2_w = ImGui::CalcTextSize("$BBBB").x;
    ImGui::TableSetupColumn("l1", ImGuiTableColumnFlags_WidthFixed, label1_w);
    ImGui::TableSetupColumn("v1", ImGuiTableColumnFlags_WidthFixed, value1_w);
    ImGui::TableSetupColumn("l2", ImGuiTableColumnFlags_WidthFixed, label2_w);
    ImGui::TableSetupColumn("v2", ImGuiTableColumnFlags_WidthFixed, value2_w);

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
}

}  // namespace pupsnes::debugger
