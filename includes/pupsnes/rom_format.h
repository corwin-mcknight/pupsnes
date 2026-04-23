#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pupsnes {

// Size of the legacy SMC copier header prepended to many .smc dumps by
// devices like the Super Magicom. The prefix is metadata for the copier
// itself and must be removed before the bytes can be mapped as a cartridge
// image.
inline constexpr std::size_t kSmcCopierHeaderSize = 512U;

// Size of a LoROM bank window. Raw LoROM images are always an integer
// multiple of this size; the 512-byte copier header is the only source of a
// sub-bank remainder we attempt to recognise here.
inline constexpr std::size_t kLoRomBankSize = 32U * 1024U;

// True when `rom_size` is consistent with a LoROM image that still carries
// an SMC copier header. The detection relies on the size-modulo test — a
// raw LoROM is always a multiple of the 32 KiB bank window, so a remainder
// of exactly 512 bytes identifies the copier prefix.
[[nodiscard]] inline bool HasSmcCopierHeader(std::size_t rom_size) noexcept {
  return (rom_size % kLoRomBankSize) == kSmcCopierHeaderSize;
}

// Removes an SMC copier header from `rom` when one is present. No-op when
// the buffer is already header-less or otherwise does not fit the detection
// rule.
inline void StripSmcCopierHeader(std::vector<uint8_t>& rom) {
  if (HasSmcCopierHeader(rom.size())) {
    rom.erase(rom.begin(), rom.begin() + static_cast<std::ptrdiff_t>(kSmcCopierHeaderSize));
  }
}

}  // namespace pupsnes
