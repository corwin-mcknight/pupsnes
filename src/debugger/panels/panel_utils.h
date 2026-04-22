#pragma once

#include "debugger/ui_utils.h"
#include "imgui.h"
#include "pupsnes/types.h"

namespace pupsnes::debugger {

// ImGui wrapper around FormatAddress24 for the common `$BB:OOOO` render case.
inline void TextAddress24(SnesAddrT address) {
  const std::string text = FormatAddress24(address);
  ImGui::TextUnformatted(text.c_str());
}

}  // namespace pupsnes::debugger
