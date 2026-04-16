#pragma once

#include "pupsnes/hw/device.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pupsnes {

class SystemBus;

class Cartridge : public Device {
  public:
    static constexpr std::size_t kLoROMWindowSize = 32U * 1024U;

    explicit Cartridge(SNES *snes);
    ~Cartridge() override = default;

    void loadLoROM(std::span<const uint8_t> rom_data);
    void mapLoROM(SystemBus &bus) const;

    [[nodiscard]] TickResult tick(time_master_delta_t budget) override;
    void onEvent(const SchedulerEvent &event) override;
    [[nodiscard]] uint8_t readRegister(uint32_t offset) override;

    [[nodiscard]] std::size_t size() const { return rom_.size(); }

  private:
    std::vector<uint8_t> rom_;
};

} // namespace pupsnes
