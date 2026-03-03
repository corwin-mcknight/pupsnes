#pragma once

#include "pupsnes/types.h"

namespace pupsnes {

class Device;

class SystemBus {
  public:
    SystemBus();
    ~SystemBus();

    [[nodiscard]] Device *getDeviceAtAddress(snes_addr_t address);
    [[nodiscard]] time_master_t getBusAccessTiming(snes_addr_t address);
};
} // namespace pupsnes
