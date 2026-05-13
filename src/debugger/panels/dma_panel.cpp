#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>

#include "debugger/app.h"
#include "debugger/time_format.h"
#include "imgui.h"
#include "panel_utils.h"
#include "panels.h"
#include "pupsnes/hw/5a22/cpu_mmio.h"
#include "pupsnes/hw/dma_controller.h"

namespace pupsnes::debugger {

namespace {

constexpr uint8_t kDmapHdmaBit = 0x40U;
constexpr uint8_t kDmapDirBit = 0x80U;
constexpr uint8_t kDmapStepMask = 0x18U;
constexpr uint8_t kDmapModeMask = 0x07U;

const char* StepLabel(uint8_t dmap) {
  switch ((dmap & kDmapStepMask) >> 3U) {
    case 0U: return "+1";
    case 1U: return "fix";
    case 2U: return "-1";
    case 3U: return "fix";
    default: return "?";
  }
}

const char* DirLabel(uint8_t dmap) { return (dmap & kDmapDirBit) != 0U ? "B->A" : "A->B"; }
bool IsHdmaChannel(uint8_t dmap) { return (dmap & kDmapHdmaBit) != 0U; }

std::string FormatMaskBits(uint8_t mask) {
  if (mask == 0U) return "none";
  std::string out;
  for (uint8_t ch = 0; ch < 8U; ++ch) {
    if ((mask & (1U << ch)) != 0U) {
      if (!out.empty()) out += ",";
      out += "ch";
      out += static_cast<char>('0' + ch);
    }
  }
  return out;
}

void DrawEnableSection(const DmaController& dma, const CpuMmio& cpu_mmio) {
  const uint8_t last_mask = dma.GetLastTriggerMask();
  if (last_mask == 0U) {
    ImGui::Text("$420B MDMAEN: 0x00  (no triggers yet)");
  } else {
    const std::string bits = FormatMaskBits(last_mask);
    ImGui::Text("$420B MDMAEN: 0x00  (last trigger mask: 0x%02X -> %s)", static_cast<unsigned>(last_mask),
                bits.c_str());
  }
  const uint8_t hdmaen = cpu_mmio.GetHdmaEn();
  const uint8_t hdma_active = dma.GetHdmaActiveMask();
  if (hdmaen == 0U && hdma_active == 0U) {
    ImGui::Text("$420C HDMAEN: 0x00  (no HDMA channels enabled)");
  } else {
    const std::string en_bits = FormatMaskBits(hdmaen);
    const std::string active_bits = FormatMaskBits(hdma_active);
    ImGui::Text("$420C HDMAEN: 0x%02X (live: %s)  | active this frame: %s", static_cast<unsigned>(hdmaen),
                en_bits.c_str(), active_bits.c_str());
  }
}

void DrawChannelRowExpansion(uint8_t ch, const DmaController::ChannelState& s) {
  ImGui::PushID(static_cast<int>(ch));
  ImGui::Indent();
  ImGui::Text("Raw $43%X0-$43%XA:", static_cast<unsigned>(ch), static_cast<unsigned>(ch));
  ImGui::Text("  DMAP=%02X  BBAD=%02X  A1TL=%02X  A1TH=%02X  A1B=%02X", s.dmap, s.bbad,
              static_cast<unsigned>(s.a1t & 0xFFU), static_cast<unsigned>((s.a1t >> 8U) & 0xFFU), s.a1b);
  ImGui::Text("  DASL=%02X  DASH=%02X  DASB=%02X  A2AL=%02X  A2AH=%02X  NTRL=%02X",
              static_cast<unsigned>(s.das & 0xFFU), static_cast<unsigned>((s.das >> 8U) & 0xFFU), s.dasb, s.a2a,
              s.a2a_high, s.ntrl);
  ImGui::Separator();
  ImGui::Text("DMAP byte decode (0x%02X):", s.dmap);
  ImGui::BulletText("bit 7 direction:    %s", DirLabel(s.dmap));
  ImGui::BulletText("bit 6 HDMA-indirect: %s", (s.dmap & 0x20U) != 0U ? "set" : "clear");
  ImGui::BulletText("bit 6 HDMA type:    %s", IsHdmaChannel(s.dmap) ? "HDMA" : "DMA");
  ImGui::BulletText("bits 4-3 A-step:    %s", StepLabel(s.dmap));
  ImGui::BulletText("bits 2-0 mode:      %u", static_cast<unsigned>(s.dmap & kDmapModeMask));
  ImGui::Unindent();
  ImGui::PopID();
}

void DrawChannelsSection(const DmaController& dma) {
  constexpr ImGuiTableFlags kFlags =
      ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
  if (!ImGui::BeginTable("dma_channels", 9, kFlags)) return;
  ImGui::TableSetupColumn("#");
  ImGui::TableSetupColumn("Type");
  ImGui::TableSetupColumn("Dir");
  ImGui::TableSetupColumn("Mode");
  ImGui::TableSetupColumn("Step");
  ImGui::TableSetupColumn("A-bus");
  ImGui::TableSetupColumn("B-port");
  ImGui::TableSetupColumn("Count");
  ImGui::TableSetupColumn("HDMA hi");
  ImGui::TableHeadersRow();

  for (uint8_t ch = 0; ch < 8U; ++ch) {
    const auto& s = dma.GetChannelState(ch);
    const bool hdma = IsHdmaChannel(s.dmap);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::PushID(static_cast<int>(ch));
    const std::string label = std::to_string(ch);
    const bool opened = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);

    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(hdma ? "HDMA" : "DMA");
    if (hdma && ImGui::IsItemHovered()) {
      if (s.hdma_finished) {
        ImGui::SetTooltip("HDMA channel finished this frame (count-byte 0 hit). Re-arms next frame.");
      } else {
        ImGui::SetTooltip("HDMA live state: A2A=$%02X%02X, NTRL=$%02X (lines left %u, repeat=%u)",
                          static_cast<unsigned>(s.a2a_high), static_cast<unsigned>(s.a2a),
                          static_cast<unsigned>(s.ntrl), static_cast<unsigned>(s.ntrl & 0x7FU),
                          static_cast<unsigned>((s.ntrl >> 7U) & 1U));
      }
    }

    ImGui::TableSetColumnIndex(2);
    ImGui::TextUnformatted(DirLabel(s.dmap));

    ImGui::TableSetColumnIndex(3);
    ImGui::Text("%u", static_cast<unsigned>(s.dmap & kDmapModeMask));

    ImGui::TableSetColumnIndex(4);
    ImGui::TextUnformatted(StepLabel(s.dmap));

    ImGui::TableSetColumnIndex(5);
    ImGui::Text("$%02X:%04X", static_cast<unsigned>(s.a1b), static_cast<unsigned>(s.a1t));

    ImGui::TableSetColumnIndex(6);
    ImGui::Text("$21%02X", static_cast<unsigned>(s.bbad));

    ImGui::TableSetColumnIndex(7);
    if (hdma) {
      ImGui::TextDisabled("---");
    } else {
      ImGui::Text("$%04X", static_cast<unsigned>(s.das));
    }

    ImGui::TableSetColumnIndex(8);
    if (hdma) {
      ImGui::Text("NTRL=%02X", static_cast<unsigned>(s.ntrl));
    } else {
      ImGui::TextDisabled("---");
    }

    if (opened) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      DrawChannelRowExpansion(ch, s);
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
  ImGui::EndTable();
}

void DrawTriggerSummary(const DmaController::TriggerRecord& rec) {
  std::string summary;
  for (uint8_t ch = 0; ch < 8U; ++ch) {
    if ((rec.channels_mask & (1U << ch)) == 0U) continue;
    if (!summary.empty()) summary += "; ";
    const auto& slot = rec.per_channel[ch];
    const bool b_to_a = (slot.dmap & kDmapDirBit) != 0U;
    const char* arrow = b_to_a ? " <- " : " -> ";
    char fragment[96];
    std::snprintf(fragment, sizeof(fragment), "ch%u: %u bytes  $%02X:%04X%s$21%02X  mode %u", static_cast<unsigned>(ch),
                  static_cast<unsigned>(slot.bytes_transferred), static_cast<unsigned>(slot.a1b),
                  static_cast<unsigned>(slot.a1t_start), arrow, static_cast<unsigned>(slot.bbad),
                  static_cast<unsigned>(slot.dmap & kDmapModeMask));
    summary += fragment;
  }
  if (summary.empty()) {
    ImGui::TextDisabled("(mask=0)");
  } else {
    ImGui::TextUnformatted(summary.c_str());
  }
}

void DrawTriggersSection(DebuggerApp& app, const DmaController& dma) {
  const std::size_t count = dma.GetRecentTriggerCount();
  if (count == 0U) {
    ImGui::TextDisabled("(no triggers yet)");
    return;
  }
  const TimeMasterT now = app.GetSnes().GetMasterTime();

  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                     ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
  if (!ImGui::BeginTable("dma_triggers", 4, kFlags)) return;
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("master_time");
  ImGui::TableSetupColumn("mask");
  ImGui::TableSetupColumn("cycles");
  ImGui::TableSetupColumn("per-channel summary", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableHeadersRow();

  // Newest-first.
  for (std::size_t i = count; i > 0; --i) {
    const auto& rec = dma.GetRecentTrigger(i - 1U);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    TextMasterTime(rec.start_time, now, app.GetUiState().time_display_mode);

    ImGui::TableSetColumnIndex(1);
    ImGui::Text("0x%02X", static_cast<unsigned>(rec.channels_mask));

    ImGui::TableSetColumnIndex(2);
    ImGui::Text("%" PRIu64, static_cast<uint64_t>(rec.end_time - rec.start_time));

    ImGui::TableSetColumnIndex(3);
    DrawTriggerSummary(rec);
  }
  ImGui::EndTable();
}

}  // namespace

void RenderDmaPanel(DebuggerApp& app) {
  ScopedPanel panel("DMA / HDMA", app.GetUiState().show_dma_panel);
  if (!panel) return;

  const DmaController& dma = app.GetSnes().GetDma();
  const CpuMmio& cpu_mmio = app.GetSnes().GetCpuMmio();

  if (ImGui::CollapsingHeader("Enable state", ImGuiTreeNodeFlags_DefaultOpen)) {
    DrawEnableSection(dma, cpu_mmio);
  }
  if (ImGui::CollapsingHeader("Channels", ImGuiTreeNodeFlags_DefaultOpen)) {
    DrawChannelsSection(dma);
  }
  if (ImGui::CollapsingHeader("Recent triggers", ImGuiTreeNodeFlags_DefaultOpen)) {
    DrawTriggersSection(app, dma);
  }
}

}  // namespace pupsnes::debugger
