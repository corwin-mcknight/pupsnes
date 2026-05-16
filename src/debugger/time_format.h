#pragma once

#include <cstdint>
#include <string>

#include "pupsnes/core/types.h"

namespace pupsnes::debugger {

enum class TimeDisplayMode : std::uint8_t {
  kAbsolute = 0,
  kOffsetFromNow = 1,
  kSinceStart = 2,
  kPpu = 3,
};

inline constexpr double kMasterClockHz = 21477272.0;
inline constexpr uint64_t kMasterCyclesPerDot = 4;
inline constexpr uint64_t kMasterCyclesPerLine = 1364;
inline constexpr uint64_t kLinesPerFrame = 262;
inline constexpr uint64_t kMasterCyclesPerFrame = kMasterCyclesPerLine * kLinesPerFrame;

struct FormattedTimes {
  std::string absolute;
  std::string offset_from_now;
  std::string since_start;
  std::string ppu;
};

FormattedTimes FormatAllMasterTime(TimeMasterT t, TimeMasterT now);
const std::string& SelectFormattedTime(const FormattedTimes& f, TimeDisplayMode mode);

const char* TimeDisplayModeShortLabel(TimeDisplayMode mode);
const char* TimeDisplayModeLongLabel(TimeDisplayMode mode);

// ImGui helper: renders the time using `mode` and shows a tooltip with all four formats on hover.
void TextMasterTime(TimeMasterT t, TimeMasterT now, TimeDisplayMode mode);

}  // namespace pupsnes::debugger
