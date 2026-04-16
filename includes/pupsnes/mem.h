#pragma once

#include "pupsnes/types.h"

namespace pupsnes {
namespace util {

constexpr SnesAddrT WrapAddr(SnesAddrT addr) { return addr & 0xFFFFFF; }

}  // namespace util
}  // namespace pupsnes