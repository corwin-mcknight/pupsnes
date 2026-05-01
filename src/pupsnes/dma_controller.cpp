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

void DmaController::WriteRegister(uint32_t /*offset*/, uint8_t /*data*/, TimeMasterT /*current_time*/) {
  // Filled in by Task 2.
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
