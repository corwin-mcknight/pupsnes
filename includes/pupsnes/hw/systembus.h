#pragma once

#include <array>
#include <cstdint>

#include "pupsnes/types.h"

namespace pupsnes {

class SNES;

enum class PageDeviceKind : uint8_t {
  kUnmapped = 0,
  kMemory = 1,
  kSameClockMmio = 2,
  kCrossClockMmio = 3,
};

struct PageTableEntry {
  DeviceIdT device_id = 0;
  uint32_t base_offset = 0;
  PageDeviceKind kind = PageDeviceKind::kUnmapped;
  uint8_t access_speed = 0;
};

enum class BusPlanOutcome : uint8_t {
  kInlineComplete = 0,
  kScheduledComplete = 1,
  kRejected = 2,
};

enum class BusAccessType : uint8_t {
  kRead = 0,
  kWrite = 1,
};

struct BusPlan {
  BusPlanOutcome outcome;
  BusAccessType access_type;
  DeviceIdT target_device;
  uint32_t device_offset;
  SnesAddrT original_address;
  TimeMasterDeltaT access_cycles;
  uint8_t write_data;
};

struct BusFollowResult {
  BusPlanOutcome outcome;
  uint8_t data;
  TokenIdT token;
};

enum class DebugAccessFailureKind : uint8_t {
  kNone = 0,
  kUnmapped = 1,
  kDeviceUnavailable = 2,
  kDeviceRefused = 3,
};

struct DebugReadResult {
  bool ok = false;
  uint8_t value = 0xFF;
  DeviceIdT device_id = 0;
  SnesAddrT address = 0;
  uint32_t device_offset = 0;
  DebugAccessFailureKind failure = DebugAccessFailureKind::kNone;
};

struct DebugWriteResult {
  bool ok = false;
  DeviceIdT device_id = 0;
  SnesAddrT address = 0;
  uint32_t device_offset = 0;
  uint8_t value = 0;
  DebugAccessFailureKind failure = DebugAccessFailureKind::kNone;
};

struct PageMapParams {
  uint8_t bank;
  uint8_t page;
  DeviceIdT device;
  uint32_t base_offset;
  PageDeviceKind kind;
  uint8_t access_speed;
};

class SystemBus {
 public:
  explicit SystemBus(SNES* snes);
  ~SystemBus() = default;

  void MapPage(const PageMapParams& params);
  void UnmapPage(uint8_t bank, uint8_t page);

  [[nodiscard]] BusPlan Plan(SnesAddrT address, BusAccessType type, uint8_t write_data = 0) const;
  BusFollowResult Follow(const BusPlan& plan, TimeMasterT current_time, DeviceIdT source_device);
  [[nodiscard]] DebugReadResult DebugRead(SnesAddrT address) const;
  [[nodiscard]] DebugWriteResult DebugWrite(SnesAddrT address, uint8_t value);

 private:
  SNES* snes_;  // Non-owning. SNES owns this SystemBus; pointer back to parent.
  uint8_t last_data_bus_value_ = 0xFF;
  using PageRow = std::array<PageTableEntry, 256>;
  std::array<PageRow, 256> page_table_{};

  BusFollowResult FollowInline(const BusPlan& plan, TimeMasterT current_time);
  BusFollowResult FollowScheduled(const BusPlan& plan, TimeMasterT current_time, DeviceIdT source_device);
  [[nodiscard]] DebugReadResult MakeDebugReadResult(const BusPlan& plan) const;
  [[nodiscard]] DebugWriteResult MakeDebugWriteResult(const BusPlan& plan, uint8_t value) const;

  friend struct SystemBusTestAccess;
};

}  // namespace pupsnes
