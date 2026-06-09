#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "pupsnes/core/device.h"

namespace pupsnes {

class SystemBus;

class WRAM : public Device {
 public:
  static constexpr std::size_t kSize = 128U * 1024U;

  explicit WRAM(SNES& snes);
  ~WRAM() override = default;

  [[nodiscard]] const char* DeviceName() const override { return "WRAM"; }

  void MapSystemBus(SystemBus& bus);

  [[nodiscard]] MmioReadResult ReadRegister(uint32_t offset, TimeMasterT current_time) override;
  void WriteRegister(uint32_t offset, uint8_t data, TimeMasterT current_time) override;
  [[nodiscard]] std::optional<uint8_t> HandleDebugRead(uint32_t offset) const override;
  bool HandleDebugWrite(uint32_t offset, uint8_t data) override;

  [[nodiscard]] uint8_t Peek(uint32_t offset) const { return bytes_[offset % kSize]; }

 private:
  std::array<uint8_t, kSize> bytes_{};
};

}  // namespace pupsnes
