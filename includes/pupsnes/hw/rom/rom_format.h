#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "pupsnes/hw/rom/cart_profile.h"

namespace pupsnes {

// Outcome of an attempt to load a cartridge image. `ok=true` means the
// cartridge state has been updated; the message carries a one-line
// detection summary. `ok=false` means the request was refused —
// `detected_kind` carries whatever the header looked like (often the OTHER
// mapper, which is the whole point of validation), and `message` explains
// specifically what was wrong.
struct RomLoadResult {
  bool ok = false;
  MapperKind detected_kind{};  // initialised to kNone (0) — see cartridge.h
  std::string message;
};

// Size of the legacy SMC copier header prepended to many .smc dumps by
// devices like the Super Magicom. The prefix is metadata for the copier
// itself and must be removed before the bytes can be mapped as a cartridge
// image.
inline constexpr std::size_t kSmcCopierHeaderSize = 512U;

// Size of a LoROM bank window. Raw LoROM images are always an integer
// multiple of this size; the 512-byte copier header is the only source of a
// sub-bank remainder we attempt to recognise here.
inline constexpr std::size_t kLoRomBankSize = 32U * 1024U;

// Size of a HiROM bank. HiROM exposes the full 64 KiB CPU bank as ROM in
// banks $40-$7D / $C0-$FF and the upper half ($8000-$FFFF) in banks
// $00-$3F / $80-$BF; both reference the same 64 KiB-strided byte offset.
inline constexpr std::size_t kHiRomBankSize = 64U * 1024U;

// File offset of the LoROM internal header. CPU address $00:FFB0-$FFDF maps
// to file offset $7FB0-$7FDF in a raw LoROM image (no SMC copier header).
inline constexpr std::size_t kLoRomHeaderOffset = 0x7FB0U;

// File offset of the SRAM size byte inside the LoROM header.
inline constexpr std::size_t kLoRomSramSizeOffset = 0x7FD8U;

// File offset of the HiROM internal header. CPU address $00:FFB0-$FFDF lands
// at file offset $FFB0-$FFDF in a raw HiROM image because HiROM exposes the
// upper half of bank $00 ($8000-$FFFF) as file bytes $8000-$FFFF.
inline constexpr std::size_t kHiRomHeaderOffset = 0xFFB0U;

// File offset of the SRAM size byte inside the HiROM header.
inline constexpr std::size_t kHiRomSramSizeOffset = 0xFFD8U;

// ExHiROM places its header in the ROM's second 4 MiB half.
inline constexpr std::size_t kExHiRomSramSizeOffset = 0x40FFD8U;

// Offsets of the map-mode byte inside each header. SNES headers encode the
// mapper in this byte:
//   $20 — LoROM (slow)        $30 — LoROM + FastROM
//   $21 — HiROM (slow)        $31 — HiROM + FastROM
//   $25 — ExHiROM (slow)      $35 — ExHiROM + FastROM
// File offsets differ by mapper; ExHiROM uses its second 4 MiB half.
inline constexpr std::size_t kLoRomMapModeOffset = 0x7FD5U;
inline constexpr std::size_t kHiRomMapModeOffset = 0xFFD5U;
inline constexpr std::size_t kExHiRomMapModeOffset = 0x40FFD5U;

// Offsets of the checksum + complement pair. Real SNES headers have the
// complement at $XXFDC and the checksum at $XXFDE; the two 16-bit words
// XORed together must equal $FFFF on a valid cart. Pirate/homebrew dumps
// often fail this; the detector treats it as a *positive* signal but never
// rejects a load purely on a checksum mismatch.
inline constexpr std::size_t kLoRomChecksumComplementOffset = 0x7FDCU;
inline constexpr std::size_t kLoRomChecksumOffset = 0x7FDEU;
inline constexpr std::size_t kHiRomChecksumComplementOffset = 0xFFDCU;
inline constexpr std::size_t kHiRomChecksumOffset = 0xFFDEU;

[[nodiscard]] constexpr bool IsLoRomMapModeByte(uint8_t value) noexcept {
  // Strict: only the two LoROM map-mode bytes count. $32 (LoROM + SA-1) is
  // intentionally excluded — the SA-1 mapper isn't implemented yet, and we
  // want the loader to refuse those ROMs with a specific message instead of
  // limping along as plain LoROM.
  return value == 0x20U || value == 0x30U;
}

[[nodiscard]] constexpr bool IsHiRomMapModeByte(uint8_t value) noexcept { return value == 0x21U || value == 0x31U; }

// Convenience accessors. Both return 0xFF (open-bus shape) when the ROM is
// too short for the header — callers can still inspect the byte without an
// extra size check.
[[nodiscard]] inline uint8_t LoRomMapModeByte(std::span<const uint8_t> rom) noexcept {
  return rom.size() > kLoRomMapModeOffset ? rom[kLoRomMapModeOffset] : 0xFFU;
}

[[nodiscard]] inline uint8_t HiRomMapModeByte(std::span<const uint8_t> rom) noexcept {
  return rom.size() > kHiRomMapModeOffset ? rom[kHiRomMapModeOffset] : 0xFFU;
}

// True when the checksum + complement at the offsets given XOR to $FFFF.
// Returns false when the ROM is too short to hold the pair.
[[nodiscard]] inline bool ChecksumPairValid(std::span<const uint8_t> rom, std::size_t complement_offset,
                                            std::size_t checksum_offset) noexcept {
  if (rom.size() <= checksum_offset + 1U) {
    return false;
  }
  const uint16_t complement = static_cast<uint16_t>(rom[complement_offset] | (rom[complement_offset + 1U] << 8U));
  const uint16_t checksum = static_cast<uint16_t>(rom[checksum_offset] | (rom[checksum_offset + 1U] << 8U));
  return static_cast<uint16_t>(complement ^ checksum) == 0xFFFFU;
}

[[nodiscard]] inline bool LoRomChecksumValid(std::span<const uint8_t> rom) noexcept {
  return ChecksumPairValid(rom, kLoRomChecksumComplementOffset, kLoRomChecksumOffset);
}

[[nodiscard]] inline bool HiRomChecksumValid(std::span<const uint8_t> rom) noexcept {
  return ChecksumPairValid(rom, kHiRomChecksumComplementOffset, kHiRomChecksumOffset);
}

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

// Returns the SRAM size in bytes declared by the HiROM internal header at
// file offset $FFD8. Same encoding and clamp as LoRomSramSize.
[[nodiscard]] inline std::size_t HiRomSramSize(std::span<const uint8_t> rom) noexcept {
  if (rom.size() <= kHiRomSramSizeOffset) {
    return 0;
  }
  const uint8_t value = rom[kHiRomSramSizeOffset];
  if (value == 0 || value > kMaxLoRomSramSizeByte) {
    return 0;
  }
  return std::size_t{1024} << value;
}

// Returns the SRAM size declared at file offset $40FFD8. Like the other
// mappers, an absent or invalid size byte means no SRAM.
[[nodiscard]] inline std::size_t ExHiRomSramSize(std::span<const uint8_t> rom) noexcept {
  if (rom.size() <= kExHiRomSramSizeOffset) {
    return 0;
  }
  const uint8_t value = rom[kExHiRomSramSizeOffset];
  if (value == 0 || value > kMaxLoRomSramSizeByte) {
    return 0;
  }
  return std::size_t{1024} << value;
}

}  // namespace pupsnes
