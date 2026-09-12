#pragma once

#include <cstdint>

#include "pupsnes/hw/5a22/cpu_mmio.h"
#include "pupsnes/hw/5a22/dma_controller.h"

namespace pupsnes::debugger {

// Show HDMA registers for a channel enabled now or initialized this frame,
// retaining its state for inspection after a mid-frame disable. DMAP's
// indirect-addressing bit does not select between general DMA and HDMA.
[[nodiscard]] inline bool ShowHdmaChannelState(const DmaController& dma, const CpuMmio& cpu_mmio, uint8_t channel) {
  const uint8_t channel_bit = static_cast<uint8_t>(1U << (channel & 7U));
  return ((cpu_mmio.GetHdmaEn() | dma.GetHdmaActiveMask()) & channel_bit) != 0U;
}

}  // namespace pupsnes::debugger
