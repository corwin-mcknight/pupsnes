#include "pupsnes/hw/cartridge.h"

#include "pupsnes/hw/systembus.h"

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

}  // namespace

Cartridge::Cartridge(SNES* snes) : Device(snes) {}

void Cartridge::LoadLoRom(std::span<const uint8_t> rom_data) { rom_.assign(rom_data.begin(), rom_data.end()); }

void Cartridge::MapLoRom(SystemBus& bus) {
  const uint8_t* const rom_data = rom_.empty() ? nullptr : rom_.data();
  const std::size_t rom_size = rom_.size();

  // Slow banks $00-$7D always tick at 8 master cycles per access. FASTROM only
  // affects the upper bank range.
  for (uint16_t bank = 0x00; bank <= 0x7D; ++bank) {
    MapLoRomBankRange(bus, GetDeviceId(), static_cast<uint8_t>(bank), rom_data, rom_size, 8);
  }

  // Fast-bank range ($80-$FD) starts in slow mode; MEMSEL will remap to 6
  // master cycles once the ROM's init code sets $420D bit 0.
  for (uint16_t bank = 0x80; bank <= 0xFD; ++bank) {
    MapLoRomBankRange(bus, GetDeviceId(), static_cast<uint8_t>(bank), rom_data, rom_size, 8);
  }

  lorom_mapped_ = true;
  mapper_kind_ = MapperKind::kLoROM;
}

void Cartridge::OnMemSelChanged(SystemBus& bus, bool fast) {
  if (!lorom_mapped_) {
    return;
  }

  const uint8_t* const rom_data = rom_.empty() ? nullptr : rom_.data();
  const std::size_t rom_size = rom_.size();
  const uint8_t access_speed = fast ? 6U : 8U;

  for (uint16_t bank = 0x80; bank <= 0xFD; ++bank) {
    MapLoRomBankRange(bus, GetDeviceId(), static_cast<uint8_t>(bank), rom_data, rom_size, access_speed);
  }
}

MmioReadResult Cartridge::ReadRegister(uint32_t offset, TimeMasterT /*current_time*/) {
  if (rom_.empty()) {
    return {0xFFU, 0xFFU};
  }

  return {rom_[static_cast<std::size_t>(offset) % rom_.size()], 0xFFU};
}

std::optional<uint8_t> Cartridge::HandleDebugRead(uint32_t offset) const {
  if (rom_.empty()) {
    return 0xFFU;
  }

  return rom_[static_cast<std::size_t>(offset) % rom_.size()];
}

}  // namespace pupsnes
