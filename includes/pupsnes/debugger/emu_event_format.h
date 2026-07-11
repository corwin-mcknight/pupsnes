#pragma once

#include <string>

#include "pupsnes/core/emu_event.h"

namespace pupsnes::debugger {

// Shared one-line text rendering for the file and console event sinks:
//   MT:<12-hex>  V:<3> H:<4>  <CATEGORY>  <NAME>  <message>
// V/H are derived from master_time with the same nominal NTSC constants the
// trace writer uses — the renderer is deterministic and side-effect-free.
[[nodiscard]] std::string FormatEmuEventLine(const EmuEvent& event);

}  // namespace pupsnes::debugger
