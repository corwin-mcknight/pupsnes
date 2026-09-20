#include "pupsnes/memory/wram.h"

#include "pupsnes/memory/systembus.h"

namespace pupsnes {

WRAM::WRAM(SNES& snes) : Device(snes) {}

MmioReadResult WRAM::ReadPort(uint16_t reg) {
  if (reg != kPortBase) return {0, 0};  // Address registers are write-only.
  const uint8_t data = bytes_[port_address_];
  port_address_ = (port_address_ + 1U) & 0x1FFFFU;
  return {data, 0xFFU};
}

void WRAM::WritePort(uint16_t reg, uint8_t data) {
  switch (reg) {
    case 0x2180:
      bytes_[port_address_] = data;
      port_address_ = (port_address_ + 1U) & 0x1FFFFU;
      break;
    case 0x2181: port_address_ = (port_address_ & 0x1FF00U) | data; break;
    case 0x2182: port_address_ = (port_address_ & 0x100FFU) | (static_cast<uint32_t>(data) << 8U); break;
    case 0x2183: port_address_ = (port_address_ & 0x0FFFFU) | ((static_cast<uint32_t>(data) & 1U) << 16U); break;
    default: break;
  }
}

void WRAM::MapSystemBus(SystemBus& bus) {
  uint8_t* const base = bytes_.data();
  for (uint16_t page = 0x00; page <= 0xFF; ++page) {
    const uint32_t page_offset = static_cast<uint32_t>(page) * 0x100U;
    bus.MapPage({0x7E, static_cast<uint8_t>(page), GetDeviceId(), page_offset, PageDeviceKind::kMemory, 8,
                 base + page_offset, base + page_offset});
    bus.MapPage({0x7F, static_cast<uint8_t>(page), GetDeviceId(), 0x10000U + page_offset, PageDeviceKind::kMemory, 8,
                 base + 0x10000U + page_offset, base + 0x10000U + page_offset});
  }

  // LowRAM mirror: the first 8 KiB of WRAM is visible at pages $00-$1F of
  // banks $00-$3F and $80-$BF. This is where the default 65C816 stack lives.
  for (uint8_t bank_base : {uint8_t{0x00U}, uint8_t{0x80U}}) {
    for (uint8_t bank_offset = 0; bank_offset < 0x40U; ++bank_offset) {
      const uint8_t mapped_bank = static_cast<uint8_t>(bank_base + bank_offset);
      for (uint16_t page = 0x00; page <= 0x1F; ++page) {
        const uint32_t page_offset = static_cast<uint32_t>(page) * 0x100U;
        bus.MapPage({mapped_bank, static_cast<uint8_t>(page), GetDeviceId(), page_offset, PageDeviceKind::kMemory, 8,
                     base + page_offset, base + page_offset});
      }
    }
  }
}

MmioReadResult WRAM::ReadRegister(uint32_t offset, TimeMasterT /*current_time*/) {
  return {bytes_[static_cast<std::size_t>(offset) % kSize], 0xFFU};
}

void WRAM::WriteRegister(uint32_t offset, uint8_t data, TimeMasterT /*current_time*/) {
  bytes_[static_cast<std::size_t>(offset) % kSize] = data;
}

std::optional<uint8_t> WRAM::HandleDebugRead(uint32_t offset) const {
  return bytes_[static_cast<std::size_t>(offset) % kSize];
}

bool WRAM::HandleDebugWrite(uint32_t offset, uint8_t data) {
  bytes_[static_cast<std::size_t>(offset) % kSize] = data;
  return true;
}

}  // namespace pupsnes
