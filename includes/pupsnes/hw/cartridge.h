#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "pupsnes/hw/device.h"

namespace pupsnes {

class SystemBus;

class Cartridge : public Device {
 public:
  static constexpr std::size_t kLoROMWindowSize = 32U * 1024U;

  explicit Cartridge(SNES* snes);
  ~Cartridge() override = default;

  void LoadLoRom(std::span<const uint8_t> rom_data);
  void MapLoRom(SystemBus& bus) const;

  [[nodiscard]] TickResult Tick(TimeMasterDeltaT budget) override;
  void OnEvent(const SchedulerEvent& event) override;
  [[nodiscard]] uint8_t ReadRegister(uint32_t offset) override;

  [[nodiscard]] std::size_t Size() const { return rom_.size(); }

 private:
  std::vector<uint8_t> rom_;
};

}  // namespace pupsnes
