#include "pupsnes/hw/wram.h"

#include "pupsnes/hw/systembus.h"

namespace pupsnes {

WRAM::WRAM(SNES* snes) : Device(snes) {}

void WRAM::MapSystemBus(SystemBus& bus) const {
  for (uint16_t page = 0x00; page <= 0xFF; ++page) {
    const uint32_t page_offset = static_cast<uint32_t>(page) * 0x100U;
    bus.MapPage({0x7E, static_cast<uint8_t>(page), GetDeviceId(), page_offset, PageDeviceKind::kMemory, 8});
    bus.MapPage({0x7F, static_cast<uint8_t>(page), GetDeviceId(), 0x10000U + page_offset, PageDeviceKind::kMemory, 8});
  }

  // LowRAM mirror: the first 8 KiB of WRAM is visible at pages $00-$1F of
  // banks $00-$3F and $80-$BF. This is where the default 65C816 stack lives.
  for (uint8_t bank_base : {uint8_t{0x00U}, uint8_t{0x80U}}) {
    for (uint8_t bank_offset = 0; bank_offset < 0x40U; ++bank_offset) {
      const uint8_t mapped_bank = static_cast<uint8_t>(bank_base + bank_offset);
      for (uint16_t page = 0x00; page <= 0x1F; ++page) {
        const uint32_t page_offset = static_cast<uint32_t>(page) * 0x100U;
        bus.MapPage({mapped_bank, static_cast<uint8_t>(page), GetDeviceId(), page_offset, PageDeviceKind::kMemory, 8});
      }
    }
  }
}

TickResult WRAM::Tick(TimeMasterDeltaT budget) { return {budget, TickStopReason::kBudgetExhausted}; }

void WRAM::OnEvent(const SchedulerEvent& /*event*/) {}

uint8_t WRAM::ReadRegister(uint32_t offset) { return bytes_[static_cast<std::size_t>(offset) % kSize]; }

void WRAM::WriteRegister(uint32_t offset, uint8_t data) { bytes_[static_cast<std::size_t>(offset) % kSize] = data; }

}  // namespace pupsnes
