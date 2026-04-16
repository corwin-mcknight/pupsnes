#include "pupsnes/hw/wram.h"

#include "pupsnes/hw/systembus.h"

namespace pupsnes {

WRAM::WRAM(SNES *snes) : Device(snes) {}

void WRAM::mapSystemBus(SystemBus &bus) const {
    for (uint16_t page = 0x00; page <= 0xFF; ++page) {
        const uint32_t page_offset = static_cast<uint32_t>(page) * 0x100U;
        bus.mapPage({0x7E, static_cast<uint8_t>(page), getDeviceId(), page_offset, PageDeviceKind::Memory, 8});
        bus.mapPage({0x7F, static_cast<uint8_t>(page), getDeviceId(), 0x10000U + page_offset, PageDeviceKind::Memory, 8});
    }
}

TickResult WRAM::tick(time_master_delta_t budget) { return {budget, TickStopReason::BudgetExhausted}; }

void WRAM::onEvent(const SchedulerEvent & /*event*/) {}

uint8_t WRAM::readRegister(uint32_t offset) { return bytes_[static_cast<std::size_t>(offset) % kSize]; }

void WRAM::writeRegister(uint32_t offset, uint8_t data) { bytes_[static_cast<std::size_t>(offset) % kSize] = data; }

} // namespace pupsnes
