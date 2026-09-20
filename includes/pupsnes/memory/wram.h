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
  static constexpr uint16_t kPortBase = 0x2180;
  static constexpr uint16_t kPortEnd = 0x2184;

  explicit WRAM(SNES& snes);
  ~WRAM() override = default;

  [[nodiscard]] const char* DeviceName() const override { return "WRAM"; }

  void MapSystemBus(SystemBus& bus);

  // B-bus ports forwarded by the page-$21 dispatcher. These share the
  // backing memory with the direct CPU mapping, with a 17-bit auto-increment.
  [[nodiscard]] MmioReadResult ReadPort(uint16_t reg);
  void WritePort(uint16_t reg, uint8_t data);
  void ResetPort() { port_address_ = 0; }

  [[nodiscard]] MmioReadResult ReadRegister(uint32_t offset, TimeMasterT current_time) override;
  void WriteRegister(uint32_t offset, uint8_t data, TimeMasterT current_time) override;
  [[nodiscard]] std::optional<uint8_t> HandleDebugRead(uint32_t offset) const override;
  bool HandleDebugWrite(uint32_t offset, uint8_t data) override;

  [[nodiscard]] uint8_t Peek(uint32_t offset) const { return bytes_[offset % kSize]; }

 private:
  std::array<uint8_t, kSize> bytes_{};
  uint32_t port_address_ = 0;
};

}  // namespace pupsnes
