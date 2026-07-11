#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "debugger/app.h"
#include "debugger/time_format.h"
#include "imgui.h"
#include "panel_utils.h"
#include "panels.h"
#include "pupsnes/debugger/emu_event_log.h"
#include "pupsnes/debugger/error_log.h"

namespace pupsnes::debugger {

namespace {

struct CategoryToggle {
  EmuEventCategory category;
  const char* label;
  ImVec4 color;
};

// Display order for the filter row and the category column tint.
constexpr std::array<CategoryToggle, 5> kCategoryToggles{{
    {EmuEventCategory::kPpu, "PPU", ImVec4(0.55F, 0.80F, 0.95F, 1.0F)},
    {EmuEventCategory::kDma, "DMA", ImVec4(0.95F, 0.80F, 0.55F, 1.0F)},
    {EmuEventCategory::kInterrupt, "IRQ", ImVec4(0.75F, 0.65F, 0.95F, 1.0F)},
    {EmuEventCategory::kError, "Errors", ImVec4(0.95F, 0.45F, 0.45F, 1.0F)},
    {EmuEventCategory::kHost, "Host", ImVec4(0.60F, 0.85F, 0.60F, 1.0F)},
}};

ImVec4 CategoryColor(EmuEventCategory category) {
  for (const CategoryToggle& toggle : kCategoryToggles) {
    if (toggle.category == category) return toggle.color;
  }
  return {1.0F, 1.0F, 1.0F, 1.0F};
}

ImVec4 SeverityColor(ErrorSeverity severity) {
  switch (severity) {
    case ErrorSeverity::kInfo: return {0.70F, 0.85F, 1.0F, 1.0F};
    case ErrorSeverity::kWarning: return {0.95F, 0.85F, 0.45F, 1.0F};
    case ErrorSeverity::kError: return {0.95F, 0.55F, 0.45F, 1.0F};
    case ErrorSeverity::kFatal: return {1.0F, 0.30F, 0.30F, 1.0F};
  }
  return {1.0F, 1.0F, 1.0F, 1.0F};
}

}  // namespace

void RenderLogPanel(DebuggerApp& app) {
  ScopedPanel panel("Log", app.GetUiState().show_log_panel);
  if (!panel) return;
  EmuEventLog& log = app.GetEmuEventLog();
  UiState& ui = app.GetUiState();

  if (ImGui::SmallButton("Clear")) {
    log.Clear();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%zu / %zu", log.Size(), log.Capacity());

  // Display-side category filter — capture stays unfiltered so toggling a
  // category back on restores its history (up to ring capacity).
  for (const CategoryToggle& toggle : kCategoryToggles) {
    ImGui::SameLine();
    const uint32_t bit = EmuEventCategoryBit(toggle.category);
    bool enabled = (ui.event_category_filter & bit) != 0U;
    if (ImGui::Checkbox(toggle.label, &enabled)) {
      ui.event_category_filter = enabled ? (ui.event_category_filter | bit) : (ui.event_category_filter & ~bit);
    }
  }

  const auto snapshot = log.Snapshot();
  std::vector<EmuEvent> filtered;
  filtered.reserve(snapshot.size());
  for (const EmuEvent& event : snapshot) {
    if ((ui.event_category_filter & EmuEventCategoryBit(EmuEventCategoryOf(event.kind))) != 0U) {
      filtered.push_back(event);
    }
  }

  const TimeMasterT now = app.GetSnes().GetMasterTime();
  const bool grew = filtered.size() > ui.event_last_seen_size;
  ui.event_last_seen_size = filtered.size();

  if (ImGui::BeginTable(
          "log_rows", 5,
          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("V/H", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Cat", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(filtered.size()));
    while (clipper.Step()) {
      for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
        const EmuEvent& event = filtered[static_cast<std::size_t>(row)];
        const EmuEventCategory category = EmuEventCategoryOf(event.kind);
        const bool is_error = category == EmuEventCategory::kError;
        ImGui::TableNextRow();
        ImGui::PushID(row);

        ImGui::TableSetColumnIndex(0);
        TextMasterTime(event.master_time, now, ui.time_display_mode);

        ImGui::TableSetColumnIndex(1);
        if (event.hv.v != kEmuEventHvUnknown) {
          // Counter position stamped at emission; H is the dot counter.
          ImGui::Text("%03u/%03u", event.hv.v, event.hv.h);
        } else {
          ImGui::TextDisabled("-");
        }

        ImGui::TableSetColumnIndex(2);
        if (is_error) {
          // Error rows surface the severity where other rows show the
          // category — the red tint already marks them as errors.
          const auto severity = static_cast<ErrorSeverity>(event.args[0]);
          ImGui::TextColored(SeverityColor(severity), "%s", ErrorSeverityName(severity));
        } else {
          ImGui::TextColored(CategoryColor(category), "%s", EmuEventCategoryName(category));
        }

        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(GetEmuEventInfo(event.kind).name);

        ImGui::TableSetColumnIndex(4);
        if (is_error) {
          const bool has_address = event.args[2] != 0U;
          if (has_address) {
            if (ImGui::SmallButton("Jump")) {
              app.JumpToAddress(event.args[1]);
            }
            ImGui::SameLine();
          }
          ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(static_cast<ErrorSeverity>(event.args[0])));
          ImGui::TextWrapped("%s", event.message.c_str());
          ImGui::PopStyleColor();
        } else {
          const std::string message = FormatEmuEventMessage(event);
          ImGui::TextUnformatted(message.c_str());
        }
        ImGui::PopID();
      }
    }

    if (grew) {
      ImGui::SetScrollHereY(1.0F);
    }
    ImGui::EndTable();
  }
}

}  // namespace pupsnes::debugger
