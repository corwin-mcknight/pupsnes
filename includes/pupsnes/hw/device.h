#pragma once

#include "pupsnes/types.h"

#include <cstdint>

namespace pupsnes {

class SNES;
struct SchedulerEvent;

enum class TickStopReason : uint8_t {
    BudgetExhausted = 0,
    BlockedOnIO = 1,
    BlockedOnToken = 2,
};
struct TickResult {
    time_master_delta_t completed_cycles;
    TickStopReason reason;
    token_id_t blocked_token = 0;
};

class Device {
  protected:
    time_master_t local_time = 0;
    SNES *snes = nullptr; // Non-owning. Owned by caller; must outlive this Device.
    device_id_t device_id_ = 0;

  public:
    explicit Device(SNES *snes);
    virtual ~Device() = default;
    [[nodiscard]] virtual TickResult tick(time_master_delta_t budget) = 0;
    virtual void onEvent(const SchedulerEvent &event) = 0;
    virtual uint8_t readRegister(uint32_t /*offset*/) { return 0; }
    virtual void writeRegister(uint32_t /*offset*/, uint8_t /*data*/) {}

    [[nodiscard]] time_master_t getTime() const { return local_time; }
    void advanceLocalTime(time_master_delta_t delta) { local_time += delta; }
    [[nodiscard]] device_id_t getDeviceId() const { return device_id_; }
};
} // namespace pupsnes
