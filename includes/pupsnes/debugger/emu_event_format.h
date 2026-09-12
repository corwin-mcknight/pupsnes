#pragma once

#include <string>

#include "pupsnes/core/emu_event.h"

namespace pupsnes::debugger {

// Shared one-line text rendering for the file and console event sinks:
//   MT:<12-hex>  V:<3> H:<3>  <CATEGORY>  <NAME>  <message>
// V/H use the counter position stamped on the event; H is measured in dots.
// Unknown positions render as dashes. Unlike the instruction trace writer,
// this formatter never derives V/H from master_time.
[[nodiscard]] std::string FormatEmuEventLine(const EmuEvent& event);

}  // namespace pupsnes::debugger
