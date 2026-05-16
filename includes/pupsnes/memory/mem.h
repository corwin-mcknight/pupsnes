#pragma once

#include "pupsnes/core/types.h"

namespace pupsnes {
namespace util {

constexpr SnesAddrT WrapAddr(SnesAddrT addr) { return addr & 0xFFFFFF; }

}  // namespace util
}  // namespace pupsnes
