#pragma once

#include <string_view>

#include "pupsnes/hw/5a22/cpu.h"

namespace pupsnes {

[[nodiscard]] std::string_view ToString(MicroBusAction action);
[[nodiscard]] std::string_view ToString(MicroInternalOp op);

}  // namespace pupsnes
