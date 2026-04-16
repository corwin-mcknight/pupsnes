#include "pupsnes/hw/cartridge.h"

#include "pupsnes/hw/systembus.h"

namespace pupsnes {

namespace {

void mapLoROMBankRange(SystemBus &bus, device_id_t device_id, uint8_t bank) {
    const uint32_t bank_offset = static_cast<uint32_t>(bank & 0x7FU) * static_cast<uint32_t>(Cartridge::kLoROMWindowSize);

    for (uint16_t page = 0x80; page <= 0xFF; ++page) {
        const uint32_t page_offset = static_cast<uint32_t>(page - 0x80U) * 0x100U;
        bus.mapPage({bank, static_cast<uint8_t>(page), device_id, bank_offset + page_offset, PageDeviceKind::Memory, 8});
    }
}

} // namespace

Cartridge::Cartridge(SNES *snes) : Device(snes) {}

void Cartridge::loadLoROM(std::span<const uint8_t> rom_data) { rom_.assign(rom_data.begin(), rom_data.end()); }

void Cartridge::mapLoROM(SystemBus &bus) const {
    for (uint16_t bank = 0x00; bank <= 0x7D; ++bank) {
        mapLoROMBankRange(bus, getDeviceId(), static_cast<uint8_t>(bank));
    }

    for (uint16_t bank = 0x80; bank <= 0xFD; ++bank) {
        mapLoROMBankRange(bus, getDeviceId(), static_cast<uint8_t>(bank));
    }
}

TickResult Cartridge::tick(time_master_delta_t budget) { return {budget, TickStopReason::BudgetExhausted}; }

void Cartridge::onEvent(const SchedulerEvent & /*event*/) {}

uint8_t Cartridge::readRegister(uint32_t offset) {
    if (rom_.empty()) {
        return 0xFFU;
    }

    return rom_[static_cast<std::size_t>(offset) % rom_.size()];
}

} // namespace pupsnes
