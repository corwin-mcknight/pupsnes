
#include "pupsnes/types.h"

namespace pupsnes {

/**
 * @class SystemBus
 * @brief Represents the main CPU bus for the system.
 *
 * The SystemBus class provides an abstraction for the main communication bus
 * used by the CPU to interact with memory and peripheral devices within the system.
 * It is responsible for coordinating data transfers and managing access to shared resources.
 */
class SystemBus {
  public:
    SystemBus();
    ~SystemBus();

    void getDeviceAtAddress(snes_addr_t address);
    time_master_t getBusAccessTiming(snes_addr_t address);
};
}; // namespace pupsnes