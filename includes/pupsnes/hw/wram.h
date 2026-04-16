#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "pupsnes/hw/device.h"

namespace pupsnes {

class SystemBus;

class WRAM : public Device {
   public:
    static constexpr std::size_t kSize = 128U * 1024U;

    explicit WRAM(SNES* snes);
    ~WRAM() override = default;

    void mapSystemBus(SystemBus& bus) const;

    [[nodiscard]] TickResult tick(time_master_delta_t budget) override;
    void onEvent(const SchedulerEvent& event) override;
    [[nodiscard]] uint8_t readRegister(uint32_t offset) override;
    void writeRegister(uint32_t offset, uint8_t data) override;

    [[nodiscard]] uint8_t peek(uint32_t offset) const { return bytes_[offset % kSize]; }

   private:
    std::array<uint8_t, kSize> bytes_{};
};

}  // namespace pupsnes
