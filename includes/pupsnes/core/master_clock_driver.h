#pragma once

#include "pupsnes/core/device.h"

namespace pupsnes {

// Marker base for devices that advance master_time by executing work (CPU,
// future DMA/HDMA). The default CatchUpTo no-op is correct because a
// master-clock driver *is* the reference; it can't fall behind itself.
class MasterClockDriver : public Device {
 public:
  using Device::Device;

  // Run until master_time >= target_master_time, or the debugger's step
  // target fires, or a breakpoint / fault triggers. The implementation
  // writes master_time directly as it retires each bus access or internal
  // micro-op. Never overshoots the target.
  [[nodiscard]] virtual TickResult TickToTarget(TimeMasterT target_master_time) = 0;
};

}  // namespace pupsnes
