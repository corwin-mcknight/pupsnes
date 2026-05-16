#pragma once

#include <format>
#include <string>

#include "pupsnes/core/types.h"

namespace pupsnes::debugger {

// Canonical "bank:offset" hex rendering for 24-bit SNES addresses ("$7E:0123").
inline std::string FormatAddress24(SnesAddrT address) {
  return std::format("${:02X}:{:04X}", static_cast<unsigned>(address >> 16), static_cast<unsigned>(address & 0xFFFFU));
}

}  // namespace pupsnes::debugger
