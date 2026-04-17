#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "pupsnes/hw/device.h"

namespace pupsnes {

class SystemBus;

class WRAM : public Device {
 public:
  static constexpr std::size_t kSize = 128U * 1024U;

  explicit WRAM(SNES* snes);
  ~WRAM() override = default;

  void MapSystemBus(SystemBus& bus) const;

  [[nodiscard]] TickResult Tick(TimeMasterDeltaT budget) override;
  void OnEvent(const SchedulerEvent& event) override;
  [[nodiscard]] uint8_t ReadRegister(uint32_t offset) override;
  void WriteRegister(uint32_t offset, uint8_t data) override;
  [[nodiscard]] std::optional<uint8_t> HandleDebugRead(uint32_t offset) const override;
  bool HandleDebugWrite(uint32_t offset, uint8_t data) override;

  [[nodiscard]] uint8_t Peek(uint32_t offset) const { return bytes_[offset % kSize]; }

 private:
  std::array<uint8_t, kSize> bytes_{};
};

}  // namespace pupsnes
