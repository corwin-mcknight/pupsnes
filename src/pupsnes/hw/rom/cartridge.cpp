#include "pupsnes/hw/rom/cartridge.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>

#include "pupsnes/memory/systembus.h"
#include "pupsnes/hw/rom/rom_format.h"

namespace pupsnes {

namespace {

// Header offsets relative to the start of the LoROM/HiROM internal header.
// Title: 21 ASCII bytes starting at $FFC0/$7FC0. Country: one byte at
// $FFD9/$7FD9. Map mode byte already lives at kLoRomMapModeOffset /
// kHiRomMapModeOffset in rom_format.h.
constexpr std::size_t kTitleLength = 21U;
constexpr std::size_t kLoRomTitleOffset = 0x7FC0U;
constexpr std::size_t kHiRomTitleOffset = 0xFFC0U;
constexpr std::size_t kLoRomCountryOffset = 0x7FD9U;
constexpr std::size_t kHiRomCountryOffset = 0xFFD9U;

}  // namespace

std::string Cartridge::GetInternalTitle() const {
  std::size_t offset = 0;
  switch (mapper_kind_) {
    case MapperKind::kLoROM: offset = kLoRomTitleOffset; break;
    case MapperKind::kHiROM: offset = kHiRomTitleOffset; break;
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

void MapLoRomBankRange(SystemBus& bus, DeviceIdT device_id, uint8_t bank, const uint8_t* rom_data, std::size_t rom_size,
                       uint8_t access_speed) {
  const uint32_t bank_offset = static_cast<uint32_t>(bank & 0x7FU) * static_cast<uint32_t>(Cartridge::kLoROMWindowSize);

  for (uint16_t page = 0x80; page <= 0xFF; ++page) {
    const uint32_t page_offset = static_cast<uint32_t>(page - 0x80U) * 0x100U;
    const uint32_t absolute_offset = bank_offset + page_offset;

    // Only enable the fast-pointer path when the whole 256-byte window lands
    // inside the ROM. Short ROMs rely on the modulo-wrap in ReadRegister, which
    // can't be expressed as a contiguous pointer window.
    const uint8_t* fast_ptr = nullptr;
    if (rom_data != nullptr && absolute_offset + 0x100U <= rom_size) {
      fast_ptr = rom_data + absolute_offset;
    }

    bus.MapPage({bank, static_cast<uint8_t>(page), device_id, absolute_offset, PageDeviceKind::kMemory, access_speed,
                 fast_ptr, nullptr});
  }
}

// Map SRAM into pages $00-$7F of a single LoROM SRAM bank. Each 256-byte page
// resolves to (page_offset % sram_size) inside the SRAM buffer, which gives
// every cartridge mirror Super Metroid relies on for its piracy check (the
// $702000 / $700000 alias) for free. Writes go through the slow path
// (fast_write_ptr = nullptr) so WriteRegister can update the dirty flag.
void MapLoRomSramBank(SystemBus& bus, DeviceIdT device_id, uint8_t bank, uint8_t* sram_data, std::size_t sram_size) {
  for (uint16_t page = 0x00; page <= 0x7F; ++page) {
    const uint32_t page_offset = static_cast<uint32_t>(page) * 0x100U;
    const uint32_t sram_offset = page_offset % static_cast<uint32_t>(sram_size);
    bus.MapPage({bank, static_cast<uint8_t>(page), device_id, Cartridge::kSramOffsetTag | sram_offset,
                 PageDeviceKind::kMemory, 8, sram_data + sram_offset, nullptr});
  }
}

// Map a contiguous page run [first_page, last_page] of a HiROM bank into the
// 64 KiB-strided ROM byte layout. bank_index_mask selects which 6 low bank
// bits feed into the 64 KiB stride; both the $00-$3F / $80-$BF half-bank
// space and the $40-$7D / $C0-$FF full-bank space share the same `(bank &
// 0x3F) << 16 | (page << 8)` mapping, so the FASTROM mirrors land on the
// same ROM bytes as the slow banks.
void MapHiRomBankPages(SystemBus& bus, DeviceIdT device_id, uint8_t bank, uint8_t first_page, uint8_t last_page,
                       const uint8_t* rom_data, std::size_t rom_size, uint8_t access_speed) {
  const uint32_t bank_offset = static_cast<uint32_t>(bank & 0x3FU) * static_cast<uint32_t>(Cartridge::kHiROMBankSize);

  for (uint16_t page = first_page; page <= last_page; ++page) {
    const uint32_t page_offset = static_cast<uint32_t>(page) * 0x100U;
    const uint32_t absolute_offset = bank_offset + page_offset;

    // Fast pointer only when the whole 256-byte window fits the ROM. Short
    // ROMs fall back to the slow path's modulo wrap (see ReadRegister).
    const uint8_t* fast_ptr = nullptr;
    if (rom_data != nullptr && absolute_offset + 0x100U <= rom_size) {
      fast_ptr = rom_data + absolute_offset;
    }

    bus.MapPage({bank, static_cast<uint8_t>(page), device_id, absolute_offset, PageDeviceKind::kMemory, access_speed,
                 fast_ptr, nullptr});
  }
}

// Map SRAM into pages $60-$7F (CPU $6000-$7FFF) of a HiROM SRAM bank. The
// HiROM SRAM window is 8 KiB per bank; smaller SRAMs mirror inside that
// window via (page_offset % sram_size), matching real hardware decoding.
void MapHiRomSramBank(SystemBus& bus, DeviceIdT device_id, uint8_t bank, uint8_t* sram_data, std::size_t sram_size) {
  for (uint16_t page = 0x60; page <= 0x7F; ++page) {
    const uint32_t window_offset = static_cast<uint32_t>(page - 0x60U) * 0x100U;
    const uint32_t sram_offset = window_offset % static_cast<uint32_t>(sram_size);
    bus.MapPage({bank, static_cast<uint8_t>(page), device_id, Cartridge::kSramOffsetTag | sram_offset,
                 PageDeviceKind::kMemory, 8, sram_data + sram_offset, nullptr});
  }
}

}  // namespace

Cartridge::Cartridge(SNES* snes) : Device(snes) {}

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
  const uint8_t* const rom_data = rom_.empty() ? nullptr : rom_.data();
  const std::size_t rom_size = rom_.size();

  // Slow banks $00-$7D always tick at 8 master cycles per access. FASTROM only
  // affects the upper bank range.
  for (uint16_t bank = 0x00; bank <= 0x7D; ++bank) {
    MapLoRomBankRange(bus, GetDeviceId(), static_cast<uint8_t>(bank), rom_data, rom_size, 8);
  }

  // Fast-bank range ($80-$FF) starts in slow mode; MEMSEL will remap to 6
  // master cycles once the ROM's init code sets $420D bit 0.
  for (uint16_t bank = 0x80; bank <= 0xFF; ++bank) {
    MapLoRomBankRange(bus, GetDeviceId(), static_cast<uint8_t>(bank), rom_data, rom_size, 8);
  }

  MapLoRomSram(bus);

  lorom_mapped_ = true;
  mapper_kind_ = MapperKind::kLoROM;
}

void Cartridge::MapLoRomSram(SystemBus& bus) {
  if (sram_.empty()) {
    return;
  }

  uint8_t* const sram_data = sram_.data();
  const std::size_t sram_size = sram_.size();

  // LoROM SRAM lives in pages $00-$7F of banks $70-$7D (banks $7E-$7F are
  // WRAM-only) and the same pages of the FASTROM-mirror banks $F0-$FF. Real
  // carts wire SRAM through both half-spaces; Super Metroid happens to use
  // bank $70 explicitly, but games that touch $F0+ rely on the upper mirror.
  for (uint16_t bank = 0x70; bank <= 0x7D; ++bank) {
    MapLoRomSramBank(bus, GetDeviceId(), static_cast<uint8_t>(bank), sram_data, sram_size);
  }
  for (uint16_t bank = 0xF0; bank <= 0xFF; ++bank) {
    MapLoRomSramBank(bus, GetDeviceId(), static_cast<uint8_t>(bank), sram_data, sram_size);
  }
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
  const uint8_t* const rom_data = rom_.empty() ? nullptr : rom_.data();
  const std::size_t rom_size = rom_.size();

  // Half-bank ROM at $00-$3F & $80-$BF lives in pages $80-$FF. Pages $00-$7F
  // are the LowRAM mirror / B-bus / CPU MMIO / (optionally) SRAM, all owned
  // by other devices that have already mapped those page-table slots in
  // SNES::SNES(). Skipping them here preserves those mappings.
  for (uint16_t bank = 0x00; bank <= 0x3F; ++bank) {
    MapHiRomBankPages(bus, GetDeviceId(), static_cast<uint8_t>(bank), 0x80U, 0xFFU, rom_data, rom_size, 8);
  }
  for (uint16_t bank = 0x80; bank <= 0xBF; ++bank) {
    // Fast-bank half lives here; starts in slow mode and MEMSEL bit 0 will
    // flip the pages we just wrote to 6 master cycles.
    MapHiRomBankPages(bus, GetDeviceId(), static_cast<uint8_t>(bank), 0x80U, 0xFFU, rom_data, rom_size, 8);
  }

  // Full-bank ROM at $40-$7D & $C0-$FF — pages $00-$FF land on contiguous
  // 64 KiB ROM windows. Banks $7E-$7F are WRAM only, so the slow-bank loop
  // stops at $7D; the FASTROM mirror $C0-$FF has no WRAM hole.
  for (uint16_t bank = 0x40; bank <= 0x7D; ++bank) {
    MapHiRomBankPages(bus, GetDeviceId(), static_cast<uint8_t>(bank), 0x00U, 0xFFU, rom_data, rom_size, 8);
  }
  for (uint16_t bank = 0xC0; bank <= 0xFF; ++bank) {
    MapHiRomBankPages(bus, GetDeviceId(), static_cast<uint8_t>(bank), 0x00U, 0xFFU, rom_data, rom_size, 8);
  }

  MapHiRomSram(bus);

  hirom_mapped_ = true;
  mapper_kind_ = MapperKind::kHiROM;
}

void Cartridge::MapHiRomSram(SystemBus& bus) {
  if (sram_.empty()) {
    return;
  }

  uint8_t* const sram_data = sram_.data();
  const std::size_t sram_size = sram_.size();

  // HiROM SRAM lives in banks $20-$3F (and the FASTROM mirror $A0-$BF) at
  // CPU $6000-$7FFF — pages $60-$7F. The half-bank ROM mapping only writes
  // pages $80-$FF, so these page-table slots are unowned until SRAM claims
  // them here.
  for (uint16_t bank = 0x20; bank <= 0x3F; ++bank) {
    MapHiRomSramBank(bus, GetDeviceId(), static_cast<uint8_t>(bank), sram_data, sram_size);
  }
  for (uint16_t bank = 0xA0; bank <= 0xBF; ++bank) {
    MapHiRomSramBank(bus, GetDeviceId(), static_cast<uint8_t>(bank), sram_data, sram_size);
  }
}

void Cartridge::OnMemSelChanged(SystemBus& bus, bool fast) {
  const uint8_t* const rom_data = rom_.empty() ? nullptr : rom_.data();
  const std::size_t rom_size = rom_.size();
  const uint8_t access_speed = fast ? 6U : 8U;

  if (lorom_mapped_) {
    for (uint16_t bank = 0x80; bank <= 0xFF; ++bank) {
      MapLoRomBankRange(bus, GetDeviceId(), static_cast<uint8_t>(bank), rom_data, rom_size, access_speed);
    }
    return;
  }

  if (hirom_mapped_) {
    // Only the ROM-bearing pages of the fast-bank half-space flip speed.
    // Half-bank mirror $80-$BF: only pages $80-$FF. The lower half is WRAM
    // mirror / MMIO / SRAM — not ours to retime.
    for (uint16_t bank = 0x80; bank <= 0xBF; ++bank) {
      MapHiRomBankPages(bus, GetDeviceId(), static_cast<uint8_t>(bank), 0x80U, 0xFFU, rom_data, rom_size, access_speed);
    }
    // Full-bank mirror $C0-$FF: all pages are ROM, so retime the whole bank.
    for (uint16_t bank = 0xC0; bank <= 0xFF; ++bank) {
      MapHiRomBankPages(bus, GetDeviceId(), static_cast<uint8_t>(bank), 0x00U, 0xFFU, rom_data, rom_size, access_speed);
    }
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
