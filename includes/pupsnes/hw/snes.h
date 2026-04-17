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

  void DebugPrintInfo();

  [[nodiscard]] TimeMasterT GetMasterTime() const { return time_now_; }
  void SetMasterTime(TimeMasterT t) { time_now_ = t; }

  DeviceIdT RegisterDevice(Device* device);
  [[nodiscard]] Device* GetDevice(DeviceIdT id) const;

 private:
  TimeMasterT time_now_ = 0;
  std::vector<Device*> devices_;  // Non-owning. Devices register themselves; caller owns them.
};
}  // namespace pupsnes
