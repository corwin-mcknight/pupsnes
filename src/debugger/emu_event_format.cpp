#include "pupsnes/debugger/emu_event_format.h"

#include <format>
#include <string>

#include "pupsnes/debugger/error_log.h"

namespace pupsnes::debugger {

namespace {

constexpr std::size_t kCategoryColumnWidth = 5;
constexpr std::size_t kNameColumnWidth = 22;

}  // namespace

std::string FormatEmuEventLine(const EmuEvent& event) {
  const EmuEventInfo& info = GetEmuEventInfo(event.kind);
  const EmuEventCategory category = EmuEventCategoryOf(event.kind);
  // V/H are the counter values stamped at emission (H is the dot counter,
  // OPHCT semantics) — never re-derived from master_time. Events without a
  // position (host, bridged errors) render dashes.
  const bool has_hv = event.hv.v != kEmuEventHvUnknown;
  const std::string v_column = has_hv ? std::format("{:03}", event.hv.v) : "---";
  const std::string h_column = has_hv ? std::format("{:03}", event.hv.h) : "---";
  std::string message = FormatEmuEventMessage(event);
  if (category == EmuEventCategory::kError) {
    message = std::format("[{}] {}", ErrorSeverityName(static_cast<ErrorSeverity>(event.args[0])), message);
  }
  return std::format("MT:{:012X}  V:{} H:{}  {:<{}}  {:<{}}  {}", event.master_time, v_column, h_column,
                     EmuEventCategoryName(category), kCategoryColumnWidth, info.name, kNameColumnWidth, message);
}

}  // namespace pupsnes::debugger
