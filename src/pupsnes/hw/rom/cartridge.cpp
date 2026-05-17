#include "pupsnes/hw/rom/cartridge.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>

#include "pupsnes/hw/rom/mapper.h"
#include "pupsnes/hw/rom/rom_format.h"
#include "pupsnes/memory/systembus.h"

namespace pupsnes {

namespace {

// Header offsets relative to the start of the LoROM/HiROM internal header.
// Title: 21 ASCII bytes starting at $FFC0/$7FC0. Country: one byte at
// $FFD9/$7FD9. Map mode byte already lives at kLoRomMapModeOffset /
// kHiRomMapModeOffset in rom_format.h.
constexpr std::size_t kTitleLength = 21U;
constexpr std::size_t kLoRomTitleOffset = 0x7FC0U;
constexpr std::size_t kHiRomTitleOffset = 0xFFC0U;
constexpr std::size_t kExHiRomTitleOffset = 0x40FFC0U;
constexpr std::size_t kLoRomCountryOffset = 0x7FD9U;
constexpr std::size_t kHiRomCountryOffset = 0xFFD9U;
constexpr std::size_t kExHiRomCountryOffset = 0x40FFD9U;

}  // namespace

std::string Cartridge::GetInternalTitle() const {
  std::size_t offset = 0;
  switch (mapper_kind_) {
    case MapperKind::kLoROM: offset = kLoRomTitleOffset; break;
    case MapperKind::kHiROM: offset = kHiRomTitleOffset; break;
    case MapperKind::kExHiROM: offset = kExHiRomTitleOffset; break;
    case MapperKind::kNone: return {};
  }
  if (rom_.size() < offset + kTitleLength) {
    return {};
  }
  std::string title(reinterpret_cast<const char*>(rom_.data() + offset), kTitleLength);
  // SNES titles are right-padded with $20 (space); trim. Strip any high-bit
  // bytes too so a shift-JIS Japanese title doesn't render as control chars.
  while (!title.empty() && (title.back() == ' ' || static_cast<uint8_t>(title.back()) >= 0x80U)) {
    title.pop_back();
  }
  return title;
}

uint8_t Cartridge::GetCountryCode() const {
  std::size_t offset = 0;
  switch (mapper_kind_) {
    case MapperKind::kLoROM: offset = kLoRomCountryOffset; break;
    case MapperKind::kHiROM: offset = kHiRomCountryOffset; break;
    case MapperKind::kExHiROM: offset = kExHiRomCountryOffset; break;
    case MapperKind::kNone: return 0xFFU;
  }
  return offset < rom_.size() ? rom_[offset] : 0xFFU;
}

bool Cartridge::IsFastRomCapable() const {
  if (mapper_kind_ == MapperKind::kNone || rom_.empty()) {
    return false;
  }
  const std::size_t offset =
      (mapper_kind_ == MapperKind::kLoROM) ? kLoRomMapModeOffset : kHiRomMapModeOffset;
  if (offset >= rom_.size()) {
    return false;
  }
  // FastROM map-mode bytes: $30 (LoROM-fast), $31 (HiROM-fast). The high
  // nibble's bit 4 is the FastROM flag; the low nibble is the mapper.
  return (rom_[offset] & 0x10U) != 0U;
}

namespace {

// Pre-flight validation shared by Cartridge::LoadLoRom / LoadHiRom. Hard
// failures fill `out` with a specific message and return false; the caller
// should NOT touch its rom_/sram_ state in that case. Soft validity (map
// mode mismatch) is checked separately so each loader can name the wrong
// mapper explicitly in its diagnostic.
[[nodiscard]] bool CheckHardRomShape(std::span<const uint8_t> rom_data, std::size_t min_header_byte,
                                     const char* mapper_label, RomLoadResult& out) {
  if (rom_data.empty()) {
    out.ok = false;
    out.detected_kind = MapperKind::kNone;
    out.message = "ROM is empty (0 bytes)";
    return false;
  }
  if (HasSmcCopierHeader(rom_data.size())) {
    out.ok = false;
    out.detected_kind = MapperKind::kNone;
    out.message = std::format(
        "ROM still has a {}-byte SMC copier header (file size {} bytes is "
        "{} bytes off a 32 KiB bank boundary). Strip the copier header "
        "with StripSmcCopierHeader before loading.",
        kSmcCopierHeaderSize, rom_data.size(), kSmcCopierHeaderSize);
    return false;
  }
  if (rom_data.size() <= min_header_byte) {
    out.ok = false;
    out.detected_kind = MapperKind::kNone;
    out.message = std::format(
        "ROM is too small for {} ({} bytes; need at least {} bytes to "
        "cover the internal header).",
        mapper_label, rom_data.size(), min_header_byte + 1U);
    return false;
  }
  return true;
}

// Returns true when this ROM is *strictly* more consistent with the other
// mapper than the requested one. "Strictly" means: the other mapper's map
// mode byte sits in the documented range AND the requested mapper's does
// NOT. We also accept a checksum match as a tiebreaker when the map mode
// alone is inconclusive (homebrew often leaves $FFD5 alone but ships valid
// checksums).
[[nodiscard]] bool OtherMapperWins(std::span<const uint8_t> rom_data, MapperKind requested) {
  const uint8_t lorom_byte = LoRomMapModeByte(rom_data);
  const uint8_t hirom_byte = HiRomMapModeByte(rom_data);
  const bool lorom_map = IsLoRomMapModeByte(lorom_byte);
  const bool hirom_map = IsHiRomMapModeByte(hirom_byte);
  const bool lorom_csum = LoRomChecksumValid(rom_data);
  const bool hirom_csum = HiRomChecksumValid(rom_data);

  if (requested == MapperKind::kLoROM) {
    if (hirom_map && !lorom_map) return true;
    if (!lorom_map && hirom_csum && !lorom_csum) return true;
  } else if (requested == MapperKind::kHiROM) {
    if (lorom_map && !hirom_map) return true;
    if (!hirom_map && lorom_csum && !hirom_csum) return true;
  }
  return false;
}

// Builds the "this looks like the wrong mapper" message — quotes both
// candidates' header bytes so the user can see what tipped the scale.
[[nodiscard]] std::string FormatWrongMapperMessage(std::span<const uint8_t> rom_data, MapperKind requested,
                                                   MapperKind detected) {
  const uint8_t lorom_byte = LoRomMapModeByte(rom_data);
  const uint8_t hirom_byte = HiRomMapModeByte(rom_data);
  const char* requested_label = (requested == MapperKind::kLoROM) ? "LoROM" : "HiROM";
  const char* detected_label = (detected == MapperKind::kLoROM) ? "LoROM" : "HiROM";
  const char* detected_loader = (detected == MapperKind::kLoROM) ? "LoadLoRom" : "LoadHiRom";
  return std::format(
      "ROM looks like {} but {} was requested (map mode byte at $7FD5=0x{:02X}, "
      "$FFD5=0x{:02X}; LoROM checksum {}, HiROM checksum {}). Use {} instead.",
      detected_label, requested_label, lorom_byte, hirom_byte, LoRomChecksumValid(rom_data) ? "ok" : "bad",
      HiRomChecksumValid(rom_data) ? "ok" : "bad", detected_loader);
}

[[nodiscard]] std::string FormatLoadSuccess(std::span<const uint8_t> rom_data, MapperKind kind) {
  const char* label = (kind == MapperKind::kLoROM) ? "LoROM" : "HiROM";
  const uint8_t mode_byte = (kind == MapperKind::kLoROM) ? LoRomMapModeByte(rom_data) : HiRomMapModeByte(rom_data);
  const bool csum = (kind == MapperKind::kLoROM) ? LoRomChecksumValid(rom_data) : HiRomChecksumValid(rom_data);
  return std::format("Loaded {} ({} bytes, map mode 0x{:02X}, checksum {})", label, rom_data.size(), mode_byte,
                     csum ? "ok" : "bad");
}

}  // namespace

Cartridge::Cartridge(SNES* snes) : Device(snes) {}
Cartridge::~Cartridge() = default;

RomLoadResult Cartridge::LoadLoRom(std::span<const uint8_t> rom_data) {
  RomLoadResult result{};
  if (!CheckHardRomShape(rom_data, kLoRomMapModeOffset, "LoROM", result)) {
    return result;
  }
  if (OtherMapperWins(rom_data, MapperKind::kLoROM)) {
    result.ok = false;
    result.detected_kind = MapperKind::kHiROM;
    result.message = FormatWrongMapperMessage(rom_data, MapperKind::kLoROM, MapperKind::kHiROM);
    return result;
  }

  rom_.assign(rom_data.begin(), rom_data.end());

  const std::size_t sram_size = LoRomSramSize(rom_data);
  sram_.assign(sram_size, 0xFFU);
  sram_dirty_ = false;

  result.ok = true;
  result.detected_kind = MapperKind::kLoROM;
  result.message = FormatLoadSuccess(rom_data, MapperKind::kLoROM);
  return result;
}

void Cartridge::MapLoRom(SystemBus& bus) {
  mapper_ = std::make_unique<LoRomMapper>(*this, bus);
  mapper_->MapInitial();
  lorom_mapped_ = true;
  mapper_kind_ = MapperKind::kLoROM;
}

RomLoadResult Cartridge::LoadHiRom(std::span<const uint8_t> rom_data) {
  RomLoadResult result{};
  if (!CheckHardRomShape(rom_data, kHiRomMapModeOffset, "HiROM", result)) {
    return result;
  }
  if (OtherMapperWins(rom_data, MapperKind::kHiROM)) {
    result.ok = false;
    result.detected_kind = MapperKind::kLoROM;
    result.message = FormatWrongMapperMessage(rom_data, MapperKind::kHiROM, MapperKind::kLoROM);
    return result;
  }

  rom_.assign(rom_data.begin(), rom_data.end());

  const std::size_t sram_size = HiRomSramSize(rom_data);
  sram_.assign(sram_size, 0xFFU);
  sram_dirty_ = false;

  result.ok = true;
  result.detected_kind = MapperKind::kHiROM;
  result.message = FormatLoadSuccess(rom_data, MapperKind::kHiROM);
  return result;
}

void Cartridge::MapHiRom(SystemBus& bus) {
  mapper_ = std::make_unique<HiRomMapper>(*this, bus);
  mapper_->MapInitial();
  hirom_mapped_ = true;
  mapper_kind_ = MapperKind::kHiROM;
}

RomLoadResult Cartridge::LoadExHiRom(std::span<const uint8_t> rom_data) {
  // ExHiROM header lives at file offset $40FFB0; the cart must be at least
  // 4 MiB + a few hundred bytes for the header to fit. Accept anything large
  // enough to host the header offset; the mapper handles smaller-half
  // mirroring for sizes below 8 MiB.
  RomLoadResult result{};
  constexpr std::size_t kExHiRomMapModeOffset = 0x40FFD5U;
  if (rom_data.empty()) {
    result.ok = false;
    result.detected_kind = MapperKind::kNone;
    result.message = "ROM is empty (0 bytes)";
    return result;
  }
  if (HasSmcCopierHeader(rom_data.size())) {
    result.ok = false;
    result.detected_kind = MapperKind::kNone;
    result.message = std::format(
        "ROM still has a {}-byte SMC copier header (file size {} bytes is "
        "{} bytes off a 32 KiB bank boundary). Strip the copier header "
        "with StripSmcCopierHeader before loading.",
        kSmcCopierHeaderSize, rom_data.size(), kSmcCopierHeaderSize);
    return result;
  }
  if (rom_data.size() <= kExHiRomMapModeOffset) {
    result.ok = false;
    result.detected_kind = MapperKind::kNone;
    result.message = std::format(
        "ROM is too small for ExHiROM ({} bytes; need at least {} bytes to "
        "cover the internal header at $40FFB0).",
        rom_data.size(), kExHiRomMapModeOffset + 1U);
    return result;
  }

  rom_.assign(rom_data.begin(), rom_data.end());

  // SRAM size byte at $40FFD8 (same encoding as LoROM/HiROM SRAM bytes).
  constexpr std::size_t kExHiRomSramSizeOffset = 0x40FFD8U;
  const uint8_t sram_byte = rom_[kExHiRomSramSizeOffset];
  const std::size_t sram_size =
      (sram_byte == 0 || sram_byte > kMaxLoRomSramSizeByte) ? 0 : (std::size_t{1024} << sram_byte);
  sram_.assign(sram_size, 0xFFU);
  sram_dirty_ = false;

  result.ok = true;
  result.detected_kind = MapperKind::kExHiROM;
  result.message =
      std::format("Loaded ExHiROM ({} bytes, map mode 0x{:02X})", rom_data.size(), rom_data[kExHiRomMapModeOffset]);
  return result;
}

void Cartridge::MapExHiRom(SystemBus& bus) {
  mapper_ = std::make_unique<ExHiRomMapper>(*this, bus);
  mapper_->MapInitial();
  exhirom_mapped_ = true;
  mapper_kind_ = MapperKind::kExHiROM;
}

void Cartridge::OnMemSelChanged(SystemBus& /*bus*/, bool fast) {
  if (mapper_) {
    mapper_->OnMemSelChanged(fast);
  }
}

void Cartridge::LoadSram(std::span<const uint8_t> data) {
  if (sram_.empty()) {
    return;
  }
  const std::size_t copy_size = std::min(data.size(), sram_.size());
  std::copy_n(data.begin(), copy_size, sram_.begin());
  if (copy_size < sram_.size()) {
    std::fill(sram_.begin() + static_cast<std::ptrdiff_t>(copy_size), sram_.end(), 0xFFU);
  }
  sram_dirty_ = false;
}

MmioReadResult Cartridge::ReadRegister(uint32_t offset, TimeMasterT /*current_time*/) {
  if ((offset & kSramOffsetTag) != 0U) {
    if (sram_.empty()) {
      return {0xFFU, 0xFFU};
    }
    const uint32_t sram_offset = offset & ~kSramOffsetTag;
    return {sram_[static_cast<std::size_t>(sram_offset) % sram_.size()], 0xFFU};
  }

  if (rom_.empty()) {
    return {0xFFU, 0xFFU};
  }

  return {rom_[static_cast<std::size_t>(offset) % rom_.size()], 0xFFU};
}

void Cartridge::WriteRegister(uint32_t offset, uint8_t data, TimeMasterT /*current_time*/) {
  if ((offset & kSramOffsetTag) == 0U || sram_.empty()) {
    return;
  }
  const uint32_t sram_offset = offset & ~kSramOffsetTag;
  sram_[static_cast<std::size_t>(sram_offset) % sram_.size()] = data;
  sram_dirty_ = true;
}

std::optional<uint8_t> Cartridge::HandleDebugRead(uint32_t offset) const {
  if ((offset & kSramOffsetTag) != 0U) {
    if (sram_.empty()) {
      return 0xFFU;
    }
    const uint32_t sram_offset = offset & ~kSramOffsetTag;
    return sram_[static_cast<std::size_t>(sram_offset) % sram_.size()];
  }

  if (rom_.empty()) {
    return 0xFFU;
  }

  return rom_[static_cast<std::size_t>(offset) % rom_.size()];
}

bool Cartridge::HandleDebugWrite(uint32_t offset, uint8_t data) {
  if ((offset & kSramOffsetTag) == 0U || sram_.empty()) {
    return false;
  }
  const uint32_t sram_offset = offset & ~kSramOffsetTag;
  sram_[static_cast<std::size_t>(sram_offset) % sram_.size()] = data;
  sram_dirty_ = true;
  return true;
}

}  // namespace pupsnes
