#pragma once

#include "pupsnes/types.h"

#include <cstdint>

namespace pupsnes {

class SNES;
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
    time_master_t local_time = 0;
    SNES *snes = nullptr;

  public:
    Device(SNES *snes) : snes(snes) {}
    virtual ~Device() = default;
    [[nodiscard]] virtual TickResult tick(time_master_delta_t budget) = 0;
    virtual void onEvent(const SchedulerEvent &event) = 0;

    [[nodiscard]] time_master_t getTime() const { return local_time; }
};
} // namespace pupsnes
