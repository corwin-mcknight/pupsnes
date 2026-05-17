#include "pupsnes/hw/rom/mapper.h"

#include <cstdint>

#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/memory/systembus.h"

namespace pupsnes {

namespace {

// Map pages $80-$FF of a single LoROM bank into the 32 KiB-strided ROM byte
// layout. Pages $00-$7F belong to other devices (LowRAM mirror, B-bus, MMIO,
// SRAM banks) and are NOT touched here. `access_speed` is the master-cycle
// cost the bus charges for each access through this bank.
void MapLoRomBankRange(SystemBus& bus, DeviceIdT device_id, uint8_t bank, const uint8_t* rom_data,
                       std::size_t rom_size, uint8_t access_speed) {
  const uint32_t bank_offset = static_cast<uint32_t>(bank & 0x7FU) * static_cast<uint32_t>(Cartridge::kLoROMWindowSize);

  for (uint16_t page = 0x80; page <= 0xFF; ++page) {
    const uint32_t page_offset = static_cast<uint32_t>(page - 0x80U) * 0x100U;
    const uint32_t absolute_offset = bank_offset + page_offset;

    // Fast-pointer path only when the whole 256-byte window lands inside
    // the ROM. Short ROMs rely on the modulo-wrap inside Cartridge::ReadRegister,
    // which can't be expressed as a contiguous pointer window.
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
// 64 KiB-strided ROM byte layout. Both the $00-$3F / $80-$BF half-bank space
// and the $40-$7D / $C0-$FF full-bank space share the same `(bank & 0x3F) <<
// 16 | (page << 8)` mapping, so the FASTROM mirrors land on the same ROM
// bytes as the slow banks.
void MapHiRomBankPages(SystemBus& bus, DeviceIdT device_id, uint8_t bank, uint8_t first_page, uint8_t last_page,
                       const uint8_t* rom_data, std::size_t rom_size, uint8_t access_speed) {
  const uint32_t bank_offset = static_cast<uint32_t>(bank & 0x3FU) * static_cast<uint32_t>(Cartridge::kHiROMBankSize);

  for (uint16_t page = first_page; page <= last_page; ++page) {
    const uint32_t page_offset = static_cast<uint32_t>(page) * 0x100U;
    const uint32_t absolute_offset = bank_offset + page_offset;

    const uint8_t* fast_ptr = nullptr;
    if (rom_data != nullptr && absolute_offset + 0x100U <= rom_size) {
      fast_ptr = rom_data + absolute_offset;
    }

    bus.MapPage({bank, static_cast<uint8_t>(page), device_id, absolute_offset, PageDeviceKind::kMemory, access_speed,
                 fast_ptr, nullptr});
  }
}

// ExHiROM lower-bank mapping. Lower banks ($00-$3F half-bank pages $80-$FF
// and $40-$7D full-bank pages $00-$FF) see ROM offsets $400000+, wrapped
// inside the smaller-half window so banks beyond the smaller half mirror
// back to its start. half_window_size is the number of bytes the smaller
// half occupies in ROM space (rom_size - 0x400000, clamped). When
// half_window_size is 0 the lower banks are unmapped (cart is HiROM-shaped
// in size; only the upper banks have content).
void MapExHiRomLowerBankPages(SystemBus& bus, DeviceIdT device_id, uint8_t bank, uint8_t first_page, uint8_t last_page,
                              const uint8_t* rom_data, std::size_t rom_size, std::size_t half_window_size,
                              uint8_t access_speed) {
  if (half_window_size == 0) {
    return;
  }
  // Bank index within the "lower half" is the low 6 bits of the bank number.
  // Both $00-$3F and $40-$7D collapse to the same logical 0..63 range.
  const uint32_t bank_index = static_cast<uint32_t>(bank & 0x3FU);
  for (uint16_t page = first_page; page <= last_page; ++page) {
    const uint32_t page_offset = static_cast<uint32_t>(page) * 0x100U;
    const uint32_t ideal_offset = bank_index * static_cast<uint32_t>(Cartridge::kHiROMBankSize) + page_offset;
    const uint32_t window_offset = ideal_offset % static_cast<uint32_t>(half_window_size);
    const uint32_t absolute_offset = 0x400000U + window_offset;

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

// ---------------------------------------------------------------------------
// LoRomMapper
// ---------------------------------------------------------------------------

void LoRomMapper::MapInitial() {
  const auto rom = cart_.RomView();
  const uint8_t* const rom_data = rom.empty() ? nullptr : rom.data();
  const std::size_t rom_size = rom.size();
  const DeviceIdT device_id = cart_.GetDeviceId();

  // Slow banks $00-$7D always tick at 8 master cycles per access. FASTROM
  // only affects the upper bank range.
  for (uint16_t bank = 0x00; bank <= 0x7D; ++bank) {
    MapLoRomBankRange(bus_, device_id, static_cast<uint8_t>(bank), rom_data, rom_size, 8);
  }

  // Fast-bank range ($80-$FF) starts in slow mode; MEMSEL will remap to 6
  // master cycles once the ROM's init code sets $420D bit 0.
  for (uint16_t bank = 0x80; bank <= 0xFF; ++bank) {
    MapLoRomBankRange(bus_, device_id, static_cast<uint8_t>(bank), rom_data, rom_size, 8);
  }

  // LoROM SRAM lives in pages $00-$7F of banks $70-$7D (banks $7E-$7F are
  // WRAM-only) and the same pages of the FASTROM-mirror banks $F0-$FF. Real
  // carts wire SRAM through both half-spaces; Super Metroid happens to use
  // bank $70 explicitly, but games that touch $F0+ rely on the upper mirror.
  uint8_t* const sram_data = cart_.SramData();
  const std::size_t sram_size = cart_.SramSize();
  if (sram_data != nullptr && sram_size > 0) {
    for (uint16_t bank = 0x70; bank <= 0x7D; ++bank) {
      MapLoRomSramBank(bus_, device_id, static_cast<uint8_t>(bank), sram_data, sram_size);
    }
    for (uint16_t bank = 0xF0; bank <= 0xFF; ++bank) {
      MapLoRomSramBank(bus_, device_id, static_cast<uint8_t>(bank), sram_data, sram_size);
    }
  }
}

void LoRomMapper::OnMemSelChanged(bool fast) {
  const auto rom = cart_.RomView();
  const uint8_t* const rom_data = rom.empty() ? nullptr : rom.data();
  const std::size_t rom_size = rom.size();
  const uint8_t access_speed = fast ? 6U : 8U;
  const DeviceIdT device_id = cart_.GetDeviceId();

  for (uint16_t bank = 0x80; bank <= 0xFF; ++bank) {
    MapLoRomBankRange(bus_, device_id, static_cast<uint8_t>(bank), rom_data, rom_size, access_speed);
  }
}

// ---------------------------------------------------------------------------
// HiRomMapper
// ---------------------------------------------------------------------------

void HiRomMapper::MapInitial() {
  const auto rom = cart_.RomView();
  const uint8_t* const rom_data = rom.empty() ? nullptr : rom.data();
  const std::size_t rom_size = rom.size();
  const DeviceIdT device_id = cart_.GetDeviceId();

  // Half-bank ROM at $00-$3F & $80-$BF lives in pages $80-$FF. Pages $00-$7F
  // are the LowRAM mirror / B-bus / CPU MMIO / (optionally) SRAM, all owned
  // by other devices that have already mapped those page-table slots in
  // SNES::SNES(). Skipping them here preserves those mappings.
  for (uint16_t bank = 0x00; bank <= 0x3F; ++bank) {
    MapHiRomBankPages(bus_, device_id, static_cast<uint8_t>(bank), 0x80U, 0xFFU, rom_data, rom_size, 8);
  }
  for (uint16_t bank = 0x80; bank <= 0xBF; ++bank) {
    MapHiRomBankPages(bus_, device_id, static_cast<uint8_t>(bank), 0x80U, 0xFFU, rom_data, rom_size, 8);
  }

  // Full-bank ROM at $40-$7D & $C0-$FF — pages $00-$FF land on contiguous
  // 64 KiB ROM windows. Banks $7E-$7F are WRAM only, so the slow-bank loop
  // stops at $7D; the FASTROM mirror $C0-$FF has no WRAM hole.
  for (uint16_t bank = 0x40; bank <= 0x7D; ++bank) {
    MapHiRomBankPages(bus_, device_id, static_cast<uint8_t>(bank), 0x00U, 0xFFU, rom_data, rom_size, 8);
  }
  for (uint16_t bank = 0xC0; bank <= 0xFF; ++bank) {
    MapHiRomBankPages(bus_, device_id, static_cast<uint8_t>(bank), 0x00U, 0xFFU, rom_data, rom_size, 8);
  }

  // HiROM SRAM lives in banks $20-$3F (and the FASTROM mirror $A0-$BF) at
  // CPU $6000-$7FFF — pages $60-$7F. The half-bank ROM mapping only writes
  // pages $80-$FF, so these page-table slots are unowned until SRAM claims
  // them here.
  uint8_t* const sram_data = cart_.SramData();
  const std::size_t sram_size = cart_.SramSize();
  if (sram_data != nullptr && sram_size > 0) {
    for (uint16_t bank = 0x20; bank <= 0x3F; ++bank) {
      MapHiRomSramBank(bus_, device_id, static_cast<uint8_t>(bank), sram_data, sram_size);
    }
    for (uint16_t bank = 0xA0; bank <= 0xBF; ++bank) {
      MapHiRomSramBank(bus_, device_id, static_cast<uint8_t>(bank), sram_data, sram_size);
    }
  }
}

void HiRomMapper::OnMemSelChanged(bool fast) {
  const auto rom = cart_.RomView();
  const uint8_t* const rom_data = rom.empty() ? nullptr : rom.data();
  const std::size_t rom_size = rom.size();
  const uint8_t access_speed = fast ? 6U : 8U;
  const DeviceIdT device_id = cart_.GetDeviceId();

  // Only ROM-bearing pages of the fast-bank half-space flip speed.
  // Half-bank mirror $80-$BF: only pages $80-$FF. The lower half is WRAM
  // mirror / MMIO / SRAM — not ours to retime.
  for (uint16_t bank = 0x80; bank <= 0xBF; ++bank) {
    MapHiRomBankPages(bus_, device_id, static_cast<uint8_t>(bank), 0x80U, 0xFFU, rom_data, rom_size, access_speed);
  }
  // Full-bank mirror $C0-$FF: all pages are ROM, retime the whole bank.
  for (uint16_t bank = 0xC0; bank <= 0xFF; ++bank) {
    MapHiRomBankPages(bus_, device_id, static_cast<uint8_t>(bank), 0x00U, 0xFFU, rom_data, rom_size, access_speed);
  }
}

// ---------------------------------------------------------------------------
// ExHiRomMapper
// ---------------------------------------------------------------------------

void ExHiRomMapper::MapInitial() {
  const auto rom = cart_.RomView();
  const uint8_t* const rom_data = rom.empty() ? nullptr : rom.data();
  const std::size_t rom_size = rom.size();
  const DeviceIdT device_id = cart_.GetDeviceId();

  // Upper banks (the "bigger half") see ROM[$000000..]. Layout is
  // bit-identical to HiROM's bigger-half mapping. FASTROM affects these.
  for (uint16_t bank = 0x80; bank <= 0xBF; ++bank) {
    MapHiRomBankPages(bus_, device_id, static_cast<uint8_t>(bank), 0x80U, 0xFFU, rom_data, rom_size, 8);
  }
  for (uint16_t bank = 0xC0; bank <= 0xFF; ++bank) {
    MapHiRomBankPages(bus_, device_id, static_cast<uint8_t>(bank), 0x00U, 0xFFU, rom_data, rom_size, 8);
  }

  // Lower banks (the "smaller half") see ROM[$400000..] with smaller-half
  // mirroring. half_window is the smaller-half size in ROM bytes — zero
  // when the cart fits inside the upper 4 MiB, which the detector should
  // have rejected as HiROM-shaped before reaching us.
  const std::size_t half_window = (rom_size > 0x400000U) ? (rom_size - 0x400000U) : 0U;
  for (uint16_t bank = 0x00; bank <= 0x3F; ++bank) {
    MapExHiRomLowerBankPages(bus_, device_id, static_cast<uint8_t>(bank), 0x80U, 0xFFU, rom_data, rom_size, half_window,
                             8);
  }
  for (uint16_t bank = 0x40; bank <= 0x7D; ++bank) {
    MapExHiRomLowerBankPages(bus_, device_id, static_cast<uint8_t>(bank), 0x00U, 0xFFU, rom_data, rom_size, half_window,
                             8);
  }

  // SRAM placement: default to HiROM positions ($20-$3F / $A0-$BF, pages
  // $60-$7F). Real ExHiROM boards (SHVC-LJ3M-01) use $80-$BF instead, but
  // we'll handle that variant only when a concrete cart needs it.
  uint8_t* const sram_data = cart_.SramData();
  const std::size_t sram_size = cart_.SramSize();
  if (sram_data != nullptr && sram_size > 0) {
    for (uint16_t bank = 0x20; bank <= 0x3F; ++bank) {
      MapHiRomSramBank(bus_, device_id, static_cast<uint8_t>(bank), sram_data, sram_size);
    }
    for (uint16_t bank = 0xA0; bank <= 0xBF; ++bank) {
      MapHiRomSramBank(bus_, device_id, static_cast<uint8_t>(bank), sram_data, sram_size);
    }
  }
}

void ExHiRomMapper::OnMemSelChanged(bool fast) {
  const auto rom = cart_.RomView();
  const uint8_t* const rom_data = rom.empty() ? nullptr : rom.data();
  const std::size_t rom_size = rom.size();
  const uint8_t access_speed = fast ? 6U : 8U;
  const DeviceIdT device_id = cart_.GetDeviceId();

  // FASTROM affects upper banks only ($80-$BF half-bank, $C0-$FF full-bank).
  // Lower banks $00-$7D always tick at 8 master cycles.
  for (uint16_t bank = 0x80; bank <= 0xBF; ++bank) {
    MapHiRomBankPages(bus_, device_id, static_cast<uint8_t>(bank), 0x80U, 0xFFU, rom_data, rom_size, access_speed);
  }
  for (uint16_t bank = 0xC0; bank <= 0xFF; ++bank) {
    MapHiRomBankPages(bus_, device_id, static_cast<uint8_t>(bank), 0x00U, 0xFFU, rom_data, rom_size, access_speed);
  }
}

}  // namespace pupsnes
