#pragma once

#include <cstdint>

namespace pupsnes {

// The SNES has two timing domains. The Master clock drives the CPU, PPU, BUS, DMA, etc. The APU has
// its own clock domain.
// We use 64-bit integers to represent time in both domains to avoid overflow issues, as they'll
// overflow in 27,200 years at full speed.
typedef uint64_t time_master_t;
typedef uint64_t time_master_delta_t;
typedef uint64_t time_apu_t;

// 24-bit SNES address. 0xWWXXYYZZ where WW is invalid, XX is bank, YYZZ is address within bank.
typedef uint32_t snes_addr_t;

} // namespace pupsnes