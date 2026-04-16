#pragma once

#include <memory>
#include <vector>

#include "pupsnes/types.h"

namespace pupsnes {
class Scheduler;
class SystemBus;
class Device;

class SNES {
   public:
    std::unique_ptr<Scheduler> scheduler;
    std::unique_ptr<SystemBus> system_bus;

    SNES();
    ~SNES();

    void debugPrintInfo();

    [[nodiscard]] time_master_t getMasterTime() const { return time_now; }
    void setMasterTime(time_master_t t) { time_now = t; }

    device_id_t registerDevice(Device* device);
    [[nodiscard]] Device* getDevice(device_id_t id) const;

   private:
    time_master_t time_now = 0;
    std::vector<Device*> devices_;  // Non-owning. Devices register themselves; caller owns them.
};
}  // namespace pupsnes
