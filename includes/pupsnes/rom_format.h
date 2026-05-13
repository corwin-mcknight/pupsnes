#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
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

// File offset of the LoROM internal header. CPU address $00:FFB0-$FFDF maps
// to file offset $7FB0-$7FDF in a raw LoROM image (no SMC copier header).
inline constexpr std::size_t kLoRomHeaderOffset = 0x7FB0U;

// File offset of the SRAM size byte inside the LoROM header.
inline constexpr std::size_t kLoRomSramSizeOffset = 0x7FD8U;

// Largest RAM-size byte we honour. The header encodes SRAM as 1024 << N
// bytes, so N=9 maps to 512 KiB — the practical ceiling for cartridge SRAM
// and well beyond Super Metroid's 8 KiB. Values above this are treated as a
// corrupt header (header byte landed in the wrong place, bogus mapper, etc.)
// and reported as "no SRAM" rather than blowing up the allocation.
inline constexpr uint8_t kMaxLoRomSramSizeByte = 9U;

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

// Returns the SRAM size in bytes declared by the LoROM internal header at
// file offset $7FD8. Returns 0 when the ROM is shorter than the header, the
// byte is zero, or the byte is out of the supported range.
[[nodiscard]] inline std::size_t LoRomSramSize(std::span<const uint8_t> rom) noexcept {
  if (rom.size() <= kLoRomSramSizeOffset) {
    return 0;
  }
  const uint8_t value = rom[kLoRomSramSizeOffset];
  if (value == 0 || value > kMaxLoRomSramSizeByte) {
    return 0;
  }
  return std::size_t{1024} << value;
}

}  // namespace pupsnes
