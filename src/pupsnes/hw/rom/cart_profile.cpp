#include "pupsnes/hw/rom/cart_profile.h"

#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <string>

#include "pupsnes/hw/rom/rom_format.h"

namespace pupsnes {

namespace {

// ExHiROM header lives at file offset $40FFB0 — bank $40 page $FF in the
// post-load address space. Same byte offsets within the header as HiROM.
constexpr std::size_t kExHiRomMapModeOffset = 0x40FFD5U;
constexpr std::size_t kExHiRomSramSizeOffset = 0x40FFD8U;
constexpr std::size_t kExHiRomChecksumComplementOffset = 0x40FFDCU;
constexpr std::size_t kExHiRomChecksumOffset = 0x40FFDEU;
constexpr std::size_t kExHiRomChipsetOffset = 0x40FFD6U;
constexpr std::size_t kExHiRomCountryOffset = 0x40FFD9U;

constexpr std::size_t kLoRomChipsetOffset = 0x7FD6U;
constexpr std::size_t kLoRomCountryOffset = 0x7FD9U;
constexpr std::size_t kLoRomSubtypeOffset = 0x7FBFU;
constexpr std::size_t kHiRomChipsetOffset = 0xFFD6U;
constexpr std::size_t kHiRomCountryOffset = 0xFFD9U;
constexpr std::size_t kHiRomSubtypeOffset = 0xFFBFU;
constexpr std::size_t kExHiRomSubtypeOffset = 0x40FFBFU;

// Map-mode byte values: $25 = ExHiROM slow, $35 = ExHiROM fast.
[[nodiscard]] constexpr bool IsExHiRomMapModeByte(uint8_t value) noexcept {
  return value == 0x25U || value == 0x35U;
}

[[nodiscard]] uint8_t ByteAt(std::span<const uint8_t> rom, std::size_t offset) noexcept {
  return offset < rom.size() ? rom[offset] : 0xFFU;
}

[[nodiscard]] bool ExHiRomChecksumValid(std::span<const uint8_t> rom) noexcept {
  return ChecksumPairValid(rom, kExHiRomChecksumComplementOffset, kExHiRomChecksumOffset);
}

[[nodiscard]] std::size_t SramSizeFromByte(uint8_t value) noexcept {
  if (value == 0 || value > kMaxLoRomSramSizeByte) {
    return 0;
  }
  return std::size_t{1024} << value;
}

// Map a $FFD6 chipset byte (with $FFBF subtype for custom Fxh) to a
// Coprocessor enum. Strict lookup against the documented bytes in
// fullsnes.html "Chipset (ROM/RAM information on cart)" — undocumented
// combinations return kNone so spurious bytes in unheadered test images
// don't get misread as a coprocessor.
[[nodiscard]] Coprocessor CoprocessorFromChipset(uint8_t chipset_byte, uint8_t subtype_byte) noexcept {
  switch (chipset_byte) {
    case 0x00U: case 0x01U: case 0x02U: return Coprocessor::kNone;  // ROM / ROM+RAM / ROM+RAM+Battery
    case 0x03U: case 0x04U: case 0x05U: case 0x06U: return Coprocessor::kDSP;
    case 0x13U: case 0x14U: case 0x15U: case 0x1AU: return Coprocessor::kGSU;
    case 0x25U: case 0x26U: return Coprocessor::kOBC1;
    case 0x32U: case 0x34U: case 0x35U: case 0x36U: return Coprocessor::kSA1;
    case 0x43U: case 0x45U: case 0x46U: return Coprocessor::kSDD1;
    case 0x55U: return Coprocessor::kSRTC;
    case 0xE3U: return Coprocessor::kSGB;
    case 0xE5U: return Coprocessor::kSatellaview;
    case 0xF3U:
    case 0xF5U:
    case 0xF6U:
    case 0xF9U:
      // Custom — $FFBF subtype byte resolves the actual chip.
      switch (subtype_byte) {
        case 0x00U: return Coprocessor::kSPC7110;
        case 0x01U: case 0x02U: return Coprocessor::kSTxxx;
        case 0x10U: return Coprocessor::kCX4;
        default: return Coprocessor::kNone;
      }
    default: return Coprocessor::kNone;
  }
}

// $FFD6 low nibble x2/x5 means battery-backed RAM. x6 = battery without
// RAM. x9 = battery + RTC. xA = battery + GSU1 fast.
[[nodiscard]] bool HasBatteryFlag(uint8_t chipset_byte) noexcept {
  const uint8_t low_nibble = chipset_byte & 0x0FU;
  return low_nibble == 0x2U || low_nibble == 0x5U || low_nibble == 0x6U || low_nibble == 0x9U || low_nibble == 0xAU;
}

// xAh = ROM+Custom+RAM+Battery+RTC-4513 OR a plain RTC in some encodings.
// x9h = co-processor + RAM + Battery + RTC. We treat 9 as the canonical RTC
// indicator and let kSRTC / kSPC7110 builders interpret further.
[[nodiscard]] bool HasRtcFlag(uint8_t chipset_byte, uint8_t subtype_byte) noexcept {
  const uint8_t low_nibble = chipset_byte & 0x0FU;
  if (low_nibble == 0x9U) {
    return true;
  }
  // Custom chipset Fxh.00h with battery + RAM is plain SPC7110; F9h.00h is
  // SPC7110+RTC. F5h.00h is SPC7110 (no RTC). 5xh is S-RTC.
  if ((chipset_byte & 0xF0U) == 0x50U) {
    return true;
  }
  if ((chipset_byte & 0xF0U) == 0xF0U && subtype_byte == 0x00U && low_nibble == 0x9U) {
    return true;
  }
  return false;
}

[[nodiscard]] Region RegionFromCountryByte(uint8_t country) noexcept {
  // NTSC: Japan (00h), USA/Canada (01h, 0Fh), Korea (0Dh), Brazil (10h, PAL-M
  // but 60 Hz). Everything else in the $00-$14 range is PAL.
  const bool is_pal = !(country == 0x00U || country == 0x01U || country == 0x0DU || country == 0x0FU ||
                        country == 0x10U);
  return Region{country, is_pal};
}

}  // namespace

CartDetection DetectCartProfile(std::span<const uint8_t> rom_data) {
  CartProfile profile;
  profile.input_size = rom_data.size();
  profile.has_smc_copier_header = HasSmcCopierHeader(rom_data.size());
  profile.lorom_map_mode_byte = LoRomMapModeByte(rom_data);
  profile.hirom_map_mode_byte = HiRomMapModeByte(rom_data);
  profile.exhirom_map_mode_byte = ByteAt(rom_data, kExHiRomMapModeOffset);
  profile.lorom_checksum_valid = LoRomChecksumValid(rom_data);
  profile.hirom_checksum_valid = HiRomChecksumValid(rom_data);
  profile.exhirom_checksum_valid = ExHiRomChecksumValid(rom_data);

  if (rom_data.empty()) {
    return CartDetection{profile, "ROM is empty (0 bytes)"};
  }
  if (profile.has_smc_copier_header) {
    return CartDetection{profile,
                         std::format("ROM still has a {}-byte SMC copier header (file size {} bytes is {} bytes "
                                     "off a 32 KiB bank boundary). Strip the copier header with "
                                     "StripSmcCopierHeader before loading.",
                                     kSmcCopierHeaderSize, rom_data.size(), kSmcCopierHeaderSize)};
  }

  // Score each candidate. Map-mode byte hit is the primary signal; checksum
  // is the tiebreaker. ExHiROM beats HiROM only when it has BOTH a valid
  // ExHiROM map-mode byte AND its header offset fits in the file — a small
  // ROM truncated before $40FFD5 cannot be ExHiROM no matter what bytes
  // happen to live elsewhere.
  const bool can_be_exhirom = rom_data.size() > kExHiRomMapModeOffset;
  const bool lorom_map = IsLoRomMapModeByte(profile.lorom_map_mode_byte);
  const bool hirom_map = IsHiRomMapModeByte(profile.hirom_map_mode_byte);
  const bool exhirom_map = can_be_exhirom && IsExHiRomMapModeByte(profile.exhirom_map_mode_byte);

  MapperKind picked = MapperKind::kNone;
  std::size_t header_chipset_offset = 0;
  std::size_t header_country_offset = 0;
  std::size_t header_sram_size_offset = 0;

  if (exhirom_map && !lorom_map && !hirom_map) {
    picked = MapperKind::kExHiROM;
  } else if (lorom_map && !hirom_map && !exhirom_map) {
    picked = MapperKind::kLoROM;
  } else if (hirom_map && !lorom_map && !exhirom_map) {
    picked = MapperKind::kHiROM;
  } else if (lorom_map || hirom_map || exhirom_map) {
    // Multiple candidates with valid map-mode bytes. Break with checksum.
    // Prefer ExHiROM only if its checksum is the unique winner — otherwise
    // collapse the choice to the simpler LoROM/HiROM pair (most homebrew
    // ROMs leave $40FFD5 looking accidentally legal).
    if (exhirom_map && profile.exhirom_checksum_valid && !profile.lorom_checksum_valid &&
        !profile.hirom_checksum_valid) {
      picked = MapperKind::kExHiROM;
    } else if (hirom_map && profile.hirom_checksum_valid && !profile.lorom_checksum_valid) {
      picked = MapperKind::kHiROM;
    } else if (lorom_map && profile.lorom_checksum_valid && !profile.hirom_checksum_valid) {
      picked = MapperKind::kLoROM;
    } else if (hirom_map) {
      picked = MapperKind::kHiROM;
    } else {
      picked = MapperKind::kLoROM;
    }
  } else {
    // No recognised map-mode byte. Fall back to checksum alone.
    if (profile.exhirom_checksum_valid && !profile.lorom_checksum_valid && !profile.hirom_checksum_valid) {
      picked = MapperKind::kExHiROM;
    } else if (profile.hirom_checksum_valid && !profile.lorom_checksum_valid) {
      picked = MapperKind::kHiROM;
    } else if (profile.lorom_checksum_valid && !profile.hirom_checksum_valid) {
      picked = MapperKind::kLoROM;
    } else {
      // No discriminator — homebrew / test ROMs without a real header almost
      // always intend LoROM since that's what the standard ld65 LoROM linker
      // config emits.
      picked = MapperKind::kLoROM;
    }
  }

  profile.mapper = picked;
  std::size_t header_subtype_offset = 0;
  switch (picked) {
    case MapperKind::kLoROM:
      header_chipset_offset = kLoRomChipsetOffset;
      header_country_offset = kLoRomCountryOffset;
      header_sram_size_offset = kLoRomSramSizeOffset;
      header_subtype_offset = kLoRomSubtypeOffset;
      profile.fastrom_capable = (profile.lorom_map_mode_byte & 0x10U) != 0U;
      break;
    case MapperKind::kHiROM:
      header_chipset_offset = kHiRomChipsetOffset;
      header_country_offset = kHiRomCountryOffset;
      header_sram_size_offset = kHiRomSramSizeOffset;
      header_subtype_offset = kHiRomSubtypeOffset;
      profile.fastrom_capable = (profile.hirom_map_mode_byte & 0x10U) != 0U;
      break;
    case MapperKind::kExHiROM:
      header_chipset_offset = kExHiRomChipsetOffset;
      header_country_offset = kExHiRomCountryOffset;
      header_sram_size_offset = kExHiRomSramSizeOffset;
      header_subtype_offset = kExHiRomSubtypeOffset;
      profile.fastrom_capable = (profile.exhirom_map_mode_byte & 0x10U) != 0U;
      break;
    case MapperKind::kNone: break;  // unreachable — picked is always one of the above above
  }

  profile.chipset_byte = ByteAt(rom_data, header_chipset_offset);
  const uint8_t subtype = ByteAt(rom_data, header_subtype_offset);
  profile.custom_subtype = subtype;
  profile.coproc = CoprocessorFromChipset(profile.chipset_byte, subtype);
  profile.has_battery = HasBatteryFlag(profile.chipset_byte);
  profile.has_rtc = HasRtcFlag(profile.chipset_byte, subtype);
  profile.sram_bytes = SramSizeFromByte(ByteAt(rom_data, header_sram_size_offset));
  profile.region = RegionFromCountryByte(ByteAt(rom_data, header_country_offset));

  return CartDetection{profile, {}};
}

}  // namespace pupsnes
