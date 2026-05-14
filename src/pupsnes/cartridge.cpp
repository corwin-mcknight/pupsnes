#include "pupsnes/hw/cartridge.h"

#include <algorithm>

#include "pupsnes/hw/systembus.h"
#include "pupsnes/rom_format.h"

namespace pupsnes {

namespace {

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

void Cartridge::LoadLoRom(std::span<const uint8_t> rom_data) {
  rom_.assign(rom_data.begin(), rom_data.end());

  const std::size_t sram_size = LoRomSramSize(rom_data);
  sram_.assign(sram_size, 0xFFU);
  sram_dirty_ = false;
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

void Cartridge::LoadHiRom(std::span<const uint8_t> rom_data) {
  rom_.assign(rom_data.begin(), rom_data.end());

  const std::size_t sram_size = HiRomSramSize(rom_data);
  sram_.assign(sram_size, 0xFFU);
  sram_dirty_ = false;
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
