#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "pupsnes/hw/device.h"

namespace pupsnes {

class SystemBus;

class DmaController : public Device {
 public:
  // Per-channel state mirrors fullsnes $43x0..$43x7. All eight bytes are R/W
  // shadow with no decode at write time — the trigger reads the live values.
  struct ChannelState {
    uint8_t dmap = 0;      // $43x0
    uint8_t bbad = 0;      // $43x1
    uint16_t a1t = 0;      // $43x2/3 (low+high)
    uint8_t a1b = 0;       // $43x4
    uint16_t das = 0;      // $43x5/6 byte counter (0=64K)
    uint8_t dasb = 0;      // $43x7 (HDMA only; preserve)
    uint8_t a2a = 0;       // $43x8 (HDMA mid-frame; preserve)
    uint8_t a2a_high = 0;  // $43x9 (HDMA mid-frame high; preserve)
    uint8_t ntrl = 0;      // $43xA (HDMA line counter; preserve)
  };

  explicit DmaController(SNES* snes);
  ~DmaController() override = default;

  void MapSystemBus(SystemBus& bus);
  void Reset();

  [[nodiscard]] MmioReadResult ReadRegister(uint32_t offset, TimeMasterT current_time) override;
  void WriteRegister(uint32_t offset, uint8_t data, TimeMasterT current_time) override;
  [[nodiscard]] std::optional<uint8_t> HandleDebugRead(uint32_t offset) const override;
  bool HandleDebugWrite(uint32_t offset, uint8_t data) override;

  // Runs general DMA for the channels selected by `channels_mask` (bit N =
  // channel N). Returns the master time after all transfers complete.
  // Performs the bus reads/writes synchronously via SystemBus.
  TimeMasterT Trigger(uint8_t channels_mask, TimeMasterT start_time);

  [[nodiscard]] const ChannelState& GetChannelState(uint8_t channel) const {
    return channels_[channel & 7U];
  }

 private:
  // Returns the live shadow byte for a valid DMA register offset
  // ($4300-$437F, local 0x0..0xA). Returns std::nullopt for the unused
  // $xB-$xF tail and for offsets outside the channel window so callers can
  // distinguish "not handled" from a real zero byte.
  [[nodiscard]] std::optional<uint8_t> ReadRegisterShadow(uint32_t offset) const;

  std::array<ChannelState, 8> channels_{};
};

}  // namespace pupsnes
