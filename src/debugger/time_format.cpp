#include "time_format.h"

#include <cstdint>
#include <format>
#include <string>

#include "imgui.h"

namespace pupsnes::debugger {

namespace {

std::string GroupThousands(uint64_t value) {
  std::string digits = std::to_string(value);
  for (int pos = static_cast<int>(digits.size()) - 3; pos > 0; pos -= 3) {
    digits.insert(static_cast<std::size_t>(pos), 1, ',');
  }
  return digits;
}

std::string FormatAbsolute(TimeMasterT t) { return GroupThousands(static_cast<uint64_t>(t)); }

std::string FormatOffset(TimeMasterT t, TimeMasterT now) {
  const auto signed_t = static_cast<int64_t>(t);
  const auto signed_now = static_cast<int64_t>(now);
  const int64_t delta = signed_t - signed_now;
  const uint64_t abs_cyc = delta < 0 ? static_cast<uint64_t>(-delta) : static_cast<uint64_t>(delta);
  const char sign = delta < 0 ? '-' : '+';
  constexpr uint64_t kCyclesPerMs = static_cast<uint64_t>(kMasterClockHz / 1000.0);
  constexpr uint64_t kCyclesPerSec = static_cast<uint64_t>(kMasterClockHz);
  if (abs_cyc < kCyclesPerMs) {
    return std::format("{}{} cyc", sign, abs_cyc);
  }
  if (abs_cyc < kCyclesPerSec) {
    const double ms = static_cast<double>(abs_cyc) / (kMasterClockHz / 1000.0);
    return std::format("{}{:.3f} ms", sign, ms);
  }
  const double s = static_cast<double>(abs_cyc) / kMasterClockHz;
  return std::format("{}{:.3f} s", sign, s);
}

std::string FormatSinceStart(TimeMasterT t) {
  const auto ms_total = static_cast<uint64_t>((static_cast<double>(t) * 1000.0) / kMasterClockHz);
  const uint64_t hours = ms_total / 3'600'000ULL;
  const uint64_t minutes = (ms_total / 60'000ULL) % 60ULL;
  const uint64_t seconds = (ms_total / 1'000ULL) % 60ULL;
  const uint64_t millis = ms_total % 1'000ULL;
  if (hours > 0) {
    return std::format("{}:{:02}:{:02}.{:03}", hours, minutes, seconds, millis);
  }
  return std::format("{}:{:02}.{:03}", minutes, seconds, millis);
}

std::string FormatPpu(TimeMasterT t) {
  const uint64_t raw = static_cast<uint64_t>(t);
  const uint64_t frame = raw / kMasterCyclesPerFrame;
  const uint64_t in_frame = raw % kMasterCyclesPerFrame;
  const uint64_t line = in_frame / kMasterCyclesPerLine;
  const uint64_t dot = (in_frame % kMasterCyclesPerLine) / kMasterCyclesPerDot;
  return std::format("{}/{:03}/{:03}", frame, line, dot);
}

}  // namespace

FormattedTimes FormatAllMasterTime(TimeMasterT t, TimeMasterT now) {
  return FormattedTimes{
      .absolute = FormatAbsolute(t),
      .offset_from_now = FormatOffset(t, now),
      .since_start = FormatSinceStart(t),
      .ppu = FormatPpu(t),
  };
}

const std::string& SelectFormattedTime(const FormattedTimes& f, TimeDisplayMode mode) {
  switch (mode) {
    case TimeDisplayMode::kAbsolute: return f.absolute;
    case TimeDisplayMode::kOffsetFromNow: return f.offset_from_now;
    case TimeDisplayMode::kSinceStart: return f.since_start;
    case TimeDisplayMode::kPpu: return f.ppu;
  }
  return f.absolute;
}

const char* TimeDisplayModeShortLabel(TimeDisplayMode mode) {
  switch (mode) {
    case TimeDisplayMode::kAbsolute: return "Abs";
    case TimeDisplayMode::kOffsetFromNow: return "Off";
    case TimeDisplayMode::kSinceStart: return "Wall";
    case TimeDisplayMode::kPpu: return "PPU";
  }
  return "?";
}

const char* TimeDisplayModeLongLabel(TimeDisplayMode mode) {
  switch (mode) {
    case TimeDisplayMode::kAbsolute: return "Absolute cycles";
    case TimeDisplayMode::kOffsetFromNow: return "Offset from now";
    case TimeDisplayMode::kSinceStart: return "Since start";
    case TimeDisplayMode::kPpu: return "PPU (frame:line:dot)";
  }
  return "?";
}

void TextMasterTime(TimeMasterT t, TimeMasterT now, TimeDisplayMode mode) {
  const FormattedTimes f = FormatAllMasterTime(t, now);
  ImGui::TextUnformatted(SelectFormattedTime(f, mode).c_str());
  if (ImGui::IsItemHovered()) {
    ImGui::BeginTooltip();
    ImGui::Text("Absolute      %s cyc", f.absolute.c_str());
    ImGui::Text("Since start   %s", f.since_start.c_str());
    ImGui::Text("Offset now    %s", f.offset_from_now.c_str());
    ImGui::Text("PPU           %s", f.ppu.c_str());
    ImGui::EndTooltip();
  }
}

}  // namespace pupsnes::debugger
