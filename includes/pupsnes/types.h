#pragma once

#include <cstdint>

namespace pupsnes {

// The SNES has two timing domains. The Master clock drives the CPU, PPU, BUS, DMA, etc. The APU has
// its own clock domain.
// We use 64-bit integers to represent time in both domains to avoid overflow issues, as they'll
// overflow in 27,200 years at full speed.
using time_master_t = uint64_t;
using time_master_delta_t = uint64_t;
using time_apu_t = uint64_t;

// 24-bit SNES address. 0xWWXXYYZZ where WW is invalid, XX is bank, YYZZ is address within bank.
using snes_addr_t = uint32_t;

// Stable device identifier assigned at registration.
using device_id_t = uint32_t;

// Stable token identifier for pending external I/O.
using token_id_t = uint64_t;

// Monotonic sequence number for scheduler event ordering tiebreaks.
using event_seq_t = uint64_t;

}  // namespace pupsnes
