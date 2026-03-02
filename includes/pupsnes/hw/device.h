#pragma once

#include "pupsnes/hw/snes.h"
#include "pupsnes/types.h"

namespace pupsnes {

struct SchedulerEvent;

enum TickStopReason : uint8_t {
    BudgetExhausted = 0,
    BlockedOnIO = 1,
};
struct TickResult {
    time_master_delta_t completedCycles;
    TickStopReason reason;
};

class Device {
  protected:
    time_master_t time_now = 0;
    SNES *snes = nullptr;

  public:
    Device(SNES *snes) : snes(snes) {}
    virtual ~Device() = default;
    virtual TickResult tick(time_master_delta_t budget) = 0;
    virtual void onEvent(const SchedulerEvent &event) = 0;

    time_master_t getTime() const { return time_now; }
};
} // namespace pupsnes
