#pragma once

#include <cstdint>

namespace pupsnes {

// The SNES has two timing domains. The Master clock drives the CPU, PPU, BUS,
// DMA, etc. The APU has its own clock domain. We use 64-bit integers to
// represent time in both domains to avoid overflow issues, as they'll overflow
// in 27,200 years at full speed.
using TimeMasterT = uint64_t;
using TimeMasterDeltaT = uint64_t;
using TimeApuT = uint64_t;

// 24-bit SNES address. 0xWWXXYYZZ where WW is invalid, XX is bank, YYZZ is
// address within bank.
using SnesAddrT = uint32_t;

// Stable device identifier assigned at registration.
using DeviceIdT = uint32_t;

// Stable token identifier for pending external I/O.
using TokenIdT = uint64_t;

// Monotonic sequence number for scheduler event ordering tiebreaks.
using EventSeqT = uint64_t;

}  // namespace pupsnes
