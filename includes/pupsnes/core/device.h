#pragma once

#include <cstdint>
#include <optional>

#include "pupsnes/core/types.h"

namespace pupsnes {

class SNES;

enum class TickStopReason : uint8_t {
  kReachedTarget = 0,       // master_time reached the requested target
  kRetiredStepTarget = 1,   // debugger step_target hit zero
  kBreakpoint = 2,          // opcode fetch at a breakpoint address
  kFault = 3,               // undefined opcode / unimplemented / bad state
};

struct TickResult {
  TimeMasterDeltaT completed_cycles;  // informational; not used for clipping
  TickStopReason reason;
};

// Result of a same-/cross-clock MMIO read returned through the bus. `value`
// carries the device's driven bits; `driven_mask` is 1 for each bit the device
// actively drives. Fast-path kMemory accesses bypass this entirely.
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
  // Calls SNES::DeregisterDevice on the way out so the SNES device list and
  // SystemBus page table forget this Device. SNES checks its `destroying_`
  // flag and skips the call during SNES teardown — see snes.cpp.
  virtual ~Device();

  // Human-readable short name used by the debugger UI to list registered
  // devices. Subclasses override with a string literal; default keeps the
  // build green if an override is missed.
  [[nodiscard]] virtual const char* DeviceName() const { return "?"; }

  // Advance this device's internal state to exactly `target` master time.
  // Passive devices override; master-clock drivers leave the default no-op
  // because they advance master_time during their own TickToTarget.
  virtual void CatchUpTo(TimeMasterT /*target*/) {}

  virtual MmioReadResult ReadRegister(uint32_t /*offset*/, TimeMasterT /*current_time*/) {
    return {0, 0x00};
  }
  virtual void WriteRegister(uint32_t /*offset*/, uint8_t /*data*/, TimeMasterT /*current_time*/) {}

  [[nodiscard]] virtual std::optional<uint8_t> HandleDebugRead(uint32_t /*offset*/) const {
    return std::nullopt;
  }
  virtual bool HandleDebugWrite(uint32_t /*offset*/, uint8_t /*data*/) { return false; }

  [[nodiscard]] TimeMasterT GetTime() const { return local_time_; }
  void AdvanceLocalTime(TimeMasterDeltaT delta) { local_time_ += delta; }
  void SetLocalTime(TimeMasterT time) { local_time_ = time; }
  void SetDeviceId(DeviceIdT id) { device_id_ = id; }
  [[nodiscard]] DeviceIdT GetDeviceId() const { return device_id_; }
};

}  // namespace pupsnes
