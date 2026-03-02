#pragma once

#include "pupsnes/types.h"

namespace pupsnes {
namespace util {

snes_addr_t wrap_addr(snes_addr_t addr) { return addr & 0xFFFFFF; }

} // namespace util
} // namespace pupsnes