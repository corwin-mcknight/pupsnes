#include "pupsnes/hw/dma_controller.h"

#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"

namespace pupsnes {

DmaController::DmaController(SNES* snes) : Device(snes) {}

void DmaController::MapSystemBus(SystemBus& bus) {
  // Page $43 covers $4300-$43FF (8 channels x 16 bytes). Mirrored across the
  // standard MMIO bank set (banks $00-$3F and $80-$BF).
  for (uint8_t bank_base : {uint8_t{0x00U}, uint8_t{0x80U}}) {
    for (uint8_t bank_offset = 0; bank_offset < 0x40U; ++bank_offset) {
      const uint8_t bank = static_cast<uint8_t>(bank_base + bank_offset);
      bus.MapPage({bank, 0x43U, GetDeviceId(), 0x4300U,
                   PageDeviceKind::kSameClockMmio, 8, nullptr, nullptr});
    }
  }
}

void DmaController::Reset() {
  channels_.fill({});
}

MmioReadResult DmaController::ReadRegister(uint32_t /*offset*/, TimeMasterT /*current_time*/) {
  // Filled in by Task 3.
  return {0x00U, 0x00U};
}

void DmaController::WriteRegister(uint32_t offset, uint8_t data, TimeMasterT /*current_time*/) {
  const uint32_t reg = offset & 0xFFFFU;
  if (reg < 0x4300U || reg >= 0x4380U) {
    // Beyond channel 7 ($4378-$437F is unused per fullsnes). Drop.
    return;
  }
  const uint8_t channel = static_cast<uint8_t>((reg >> 4U) & 0x07U);
  const uint8_t local = static_cast<uint8_t>(reg & 0x0FU);
  ChannelState& ch = channels_[channel];
  switch (local) {
    case 0x0: ch.dmap = data; break;
    case 0x1: ch.bbad = data; break;
    case 0x2: ch.a1t = static_cast<uint16_t>((ch.a1t & 0xFF00U) | data); break;
    case 0x3: ch.a1t = static_cast<uint16_t>((ch.a1t & 0x00FFU) | (static_cast<uint32_t>(data) << 8U)); break;
    case 0x4: ch.a1b = data; break;
    case 0x5: ch.das = static_cast<uint16_t>((ch.das & 0xFF00U) | data); break;
    case 0x6: ch.das = static_cast<uint16_t>((ch.das & 0x00FFU) | (static_cast<uint32_t>(data) << 8U)); break;
    case 0x7: ch.dasb = data; break;
    case 0x8: ch.a2a = data; break;
    case 0x9: ch.a2a_high = data; break;
    case 0xA: ch.ntrl = data; break;
    default:  // $43xB-$43xF unused per fullsnes; drop.
      break;
  }
}

TimeMasterT DmaController::Trigger(uint8_t /*channels_mask*/, TimeMasterT start_time) {
  // Filled in by Task 5+.
  return start_time;
}

std::optional<uint8_t> DmaController::HandleDebugRead(uint32_t /*offset*/) const {
  // Channel-state shadow exposure to the debugger lands with Task 3. Until then
  // return a defined zero so DebugRead succeeds (and the bus marks the page
  // mapped) instead of reporting kDeviceRefused.
  return 0x00U;
}

bool DmaController::HandleDebugWrite(uint32_t /*offset*/, uint8_t /*data*/) {
  // Accept silently for now; Task 2 wires writes through to the channel shadow.
  return true;
}

}  // namespace pupsnes
