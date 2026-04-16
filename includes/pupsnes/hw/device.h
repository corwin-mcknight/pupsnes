#pragma once

#include <cstdint>
#include <optional>

#include "pupsnes/types.h"

namespace pupsnes {

class SNES;
struct SchedulerEvent;

enum class TickStopReason : uint8_t {
    kBudgetExhausted = 0,
    kReachedLocalBoundary = 1,
    kBlockedOnToken = 2,
    kNoWork = 3,
};
struct TickResult {
    TimeMasterDeltaT completed_cycles;
    TickStopReason reason;

    // Only meaningful when reason == TickStopReason::kBlockedOnToken.
    TokenIdT blocked_token = 0;

    // Absolute master-cycle wake time. Only meaningful when:
    // - reason == TickStopReason::kReachedLocalBoundary
    // - reason == TickStopReason::kNoWork (optional)
    //
    // Wake times earlier than the device's committed local_time are scheduler bugs.
    std::optional<TimeMasterT> next_wake_time = std::nullopt;
};

class Device {
   protected:
    TimeMasterT local_time_ = 0;
    SNES* snes_ = nullptr;  // Non-owning. Owned by caller; must outlive this Device.
    DeviceIdT device_id_ = 0;

   public:
    explicit Device(SNES* snes);
    virtual ~Device() = default;
    [[nodiscard]] virtual TickResult Tick(TimeMasterDeltaT budget) = 0;
    virtual void OnEvent(const SchedulerEvent& event) = 0;
    virtual uint8_t ReadRegister(uint32_t /*offset*/) { return 0; }
    virtual void WriteRegister(uint32_t /*offset*/, uint8_t /*data*/) {}

    [[nodiscard]] TimeMasterT GetTime() const { return local_time_; }
    void AdvanceLocalTime(TimeMasterDeltaT delta) { local_time_ += delta; }
    [[nodiscard]] DeviceIdT GetDeviceId() const { return device_id_; }
};
}  // namespace pupsnes
