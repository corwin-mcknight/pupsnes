#pragma once

#include <cstdint>
#include <limits>
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
  kFaulted = 4,
  // Debugger-driven stops. The CPU surfaces these directly from its Tick loop
  // when the active DebuggerContract signals a breakpoint hit or a step-count
  // boundary, respectively. Scheduler treats them like kFaulted (no wake, no
  // token, no auto-reschedule) — RunControl owns what to do next.
  kDebuggerBreakpoint = 5,
  kDebuggerStepComplete = 6,
  // Sentinel used internally by device implementations to signal "no stop yet"
  // when returning TickResult from per-cycle helpers without wrapping in
  // std::optional. Never surfaced to the scheduler.
  kContinue = 255,
};

// Sentinel for TickResult::next_wake_time meaning "no wake time provided".
// Using a sentinel instead of std::optional avoids constructing an engaged-bit
// flag on every TickResult returned from the CPU hot path.
inline constexpr TimeMasterT kNoWakeTime = std::numeric_limits<TimeMasterT>::max();

struct TickResult {
  TimeMasterDeltaT completed_cycles;
  TickStopReason reason;

  // Only meaningful when reason == TickStopReason::kBlockedOnToken.
  TokenIdT blocked_token = 0;

  // Absolute master-cycle wake time. Only meaningful when:
  // - reason == TickStopReason::kReachedLocalBoundary
  // - reason == TickStopReason::kNoWork (optional)
  //
  // kNoWakeTime means "not provided". Wake times earlier than the device's
  // committed local_time are scheduler bugs.
  TimeMasterT next_wake_time = kNoWakeTime;

  [[nodiscard]] bool Stopped() const { return reason != TickStopReason::kContinue; }
  [[nodiscard]] bool HasWakeTime() const { return next_wake_time != kNoWakeTime; }
};

// Result of a same-/cross-clock MMIO read returned through the bus. `value`
// carries the device's driven bits; `driven_mask` is 1 for each bit the device
// actively drives. The bus merges open-bus for any bit where the mask is 0:
//   merged = (value & driven_mask) | (last_data_bus_value & ~driven_mask)
// Fully-driven registers return {value, 0xFF}; pure open-bus / unimplemented
// reads return {0, 0x00}. Fast-path kMemory accesses bypass this entirely and
// are always fully driven.
struct MmioReadResult {
  uint8_t value;
  uint8_t driven_mask;
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
  virtual MmioReadResult ReadRegister(uint32_t /*offset*/, TimeMasterT /*current_time*/) { return {0, 0x00}; }
  virtual void WriteRegister(uint32_t /*offset*/, uint8_t /*data*/, TimeMasterT /*current_time*/) {}
  [[nodiscard]] virtual std::optional<uint8_t> HandleDebugRead(uint32_t /*offset*/) const { return std::nullopt; }
  virtual bool HandleDebugWrite(uint32_t /*offset*/, uint8_t /*data*/) { return false; }

  [[nodiscard]] TimeMasterT GetTime() const { return local_time_; }
  void AdvanceLocalTime(TimeMasterDeltaT delta) { local_time_ += delta; }
  void SetLocalTime(TimeMasterT time) { local_time_ = time; }
  [[nodiscard]] DeviceIdT GetDeviceId() const { return device_id_; }
};
}  // namespace pupsnes
