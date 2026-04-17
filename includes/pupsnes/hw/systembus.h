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

 private:
  SNES* snes_;  // Non-owning. SNES owns this SystemBus; pointer back to parent.
  uint8_t last_data_bus_value_ = 0xFF;
  using PageRow = std::array<PageTableEntry, 256>;
  std::array<PageRow, 256> page_table_{};

  BusFollowResult FollowInline(const BusPlan& plan, TimeMasterT current_time);
  BusFollowResult FollowScheduled(const BusPlan& plan, TimeMasterT current_time, DeviceIdT source_device);

  friend struct SystemBusTestAccess;
};

}  // namespace pupsnes
