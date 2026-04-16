#pragma once

#include "pupsnes/types.h"

namespace pupsnes {
namespace util {

constexpr snes_addr_t wrapAddr(snes_addr_t addr) { return addr & 0xFFFFFF; }

}  // namespace util
}  // namespace pupsnes