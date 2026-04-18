#include "pupsnes/hw/cartridge.h"

#include "pupsnes/hw/systembus.h"

namespace pupsnes {

namespace {

void MapLoRomBankRange(SystemBus& bus, DeviceIdT device_id, uint8_t bank, const uint8_t* rom_data,
                       std::size_t rom_size) {
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

    bus.MapPage(
        {bank, static_cast<uint8_t>(page), device_id, absolute_offset, PageDeviceKind::kMemory, 8, fast_ptr, nullptr});
  }
}

}  // namespace

Cartridge::Cartridge(SNES* snes) : Device(snes) {}

void Cartridge::LoadLoRom(std::span<const uint8_t> rom_data) { rom_.assign(rom_data.begin(), rom_data.end()); }

void Cartridge::MapLoRom(SystemBus& bus) const {
  const uint8_t* const rom_data = rom_.empty() ? nullptr : rom_.data();
  const std::size_t rom_size = rom_.size();

  for (uint16_t bank = 0x00; bank <= 0x7D; ++bank) {
    MapLoRomBankRange(bus, GetDeviceId(), static_cast<uint8_t>(bank), rom_data, rom_size);
  }

  for (uint16_t bank = 0x80; bank <= 0xFD; ++bank) {
    MapLoRomBankRange(bus, GetDeviceId(), static_cast<uint8_t>(bank), rom_data, rom_size);
  }
}

TickResult Cartridge::Tick(TimeMasterDeltaT budget) { return {budget, TickStopReason::kBudgetExhausted}; }

void Cartridge::OnEvent(const SchedulerEvent& /*event*/) {}

uint8_t Cartridge::ReadRegister(uint32_t offset) {
  if (rom_.empty()) {
    return 0xFFU;
  }

  return rom_[static_cast<std::size_t>(offset) % rom_.size()];
}

std::optional<uint8_t> Cartridge::HandleDebugRead(uint32_t offset) const {
  if (rom_.empty()) {
    return 0xFFU;
  }

  return rom_[static_cast<std::size_t>(offset) % rom_.size()];
}

}  // namespace pupsnes
