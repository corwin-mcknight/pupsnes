#pragma once

#include <array>
#include <cstdint>

#include "pupsnes/hw/bus_event.h"
#include "pupsnes/types.h"

namespace pupsnes {

class SNES;

enum class PageDeviceKind : uint8_t {
  kUnmapped = 0,
  // Pure backing storage: no device-side state, no catch-up, no arbitration.
  // Access cost is fully captured by access_speed. Enables the fast-pointer
  // read/write path that bypasses Device::ReadRegister / WriteRegister.
  // Mappers MUST NOT use this kind for any region with side effects, banking
  // registers, another bus master, or contended access — use kSameClockMmio,
  // kCrossClockMmio, or kArbitrated instead.
  kMemory = 1,
  kSameClockMmio = 2,
  kCrossClockMmio = 3,
  // Storage-backed region contended between multiple bus masters (e.g. SA-1 /
  // SuperFX shared SRAM). Always routed through Follow so the scheduler can
  // arbitrate ownership before the access completes. Reserved for future
  // mappers; no users today.
  kArbitrated = 4,
};

struct PageTableEntry {
  DeviceIdT device_id = 0;
  uint32_t base_offset = 0;
  PageDeviceKind kind = PageDeviceKind::kUnmapped;
  uint8_t access_speed = 0;
  // For kMemory pages, points at the 256-byte page window inside the device's
  // backing store. Indexed by (address & 0xFF). Null disables the fast path
  // (falls through to Device::ReadRegister / WriteRegister).
  const uint8_t* fast_read_ptr = nullptr;
  uint8_t* fast_write_ptr = nullptr;
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
  // Optional: for kMemory pages, hands the bus a direct pointer to the 256-byte
  // page window in the device's backing store so reads/writes can skip virtual
  // dispatch. Null means fall back to Device::ReadRegister / WriteRegister.
  const uint8_t* fast_read_ptr = nullptr;
  uint8_t* fast_write_ptr = nullptr;
};

class SystemBus {
 public:
  explicit SystemBus(SNES* snes);
  ~SystemBus() = default;

  void MapPage(const PageMapParams& params);
  void UnmapPage(uint8_t bank, uint8_t page);

  [[nodiscard]] BusPlan Plan(SnesAddrT address, BusAccessType type, uint8_t write_data = 0) const;
  BusFollowResult Follow(const BusPlan& plan, TimeMasterT current_time, DeviceIdT source_device);

  // Attach an optional sink to receive a notification for every bus
  // transaction (fast and slow path). Null disables recording and reduces the
  // fast-path cost to one predicted-not-taken branch.
  void SetEventSink(BusEventSink* sink) { event_sink_ = sink; }
  [[nodiscard]] BusEventSink* GetEventSink() const { return event_sink_; }
  [[nodiscard]] DebugReadResult DebugRead(SnesAddrT address) const;
  [[nodiscard]] DebugWriteResult DebugWrite(SnesAddrT address, uint8_t value);

  // Hot-path shortcut for kMemory pages with a direct pointer. Returns true
  // when handled inline (out_data is set, open-bus updated); false means the
  // caller must fall back to Plan+Follow (unmapped page, MMIO, arbitrated, or
  // test device with no fast pointer). Semantics match the kMemory branch of
  // FollowInline exactly.
  //
  // out_access_cycles receives the page's access_speed (6/8/12 master cycles)
  // so the caller can advance its time cursor. Only written when the fast path
  // succeeds; unspecified on false.
  [[nodiscard]] bool TryFastRead(SnesAddrT address, uint8_t& out_data, TimeMasterDeltaT& out_access_cycles) {
    const SnesAddrT addr = address & 0xFFFFFFU;
    const PageTableEntry& entry =
        page_table_[static_cast<uint8_t>(addr >> 16)][static_cast<uint8_t>((addr >> 8) & 0xFF)];
    if (entry.kind != PageDeviceKind::kMemory || entry.fast_read_ptr == nullptr) {
      return false;
    }
    const uint8_t data = entry.fast_read_ptr[addr & 0xFFU];
    last_data_bus_value_ = data;
    out_data = data;
    out_access_cycles = entry.access_speed;
    if (event_sink_ != nullptr) {
      NotifyEvent(BusEventKind::kFastRead, addr, data);
    }
    return true;
  }
  [[nodiscard]] bool TryFastWrite(SnesAddrT address, uint8_t data, TimeMasterDeltaT& out_access_cycles) {
    const SnesAddrT addr = address & 0xFFFFFFU;
    const PageTableEntry& entry =
        page_table_[static_cast<uint8_t>(addr >> 16)][static_cast<uint8_t>((addr >> 8) & 0xFF)];
    if (entry.kind != PageDeviceKind::kMemory || entry.fast_write_ptr == nullptr) {
      return false;
    }
    entry.fast_write_ptr[addr & 0xFFU] = data;
    last_data_bus_value_ = data;
    out_access_cycles = entry.access_speed;
    if (event_sink_ != nullptr) {
      NotifyEvent(BusEventKind::kFastWrite, addr, data);
    }
    return true;
  }

 private:
  SNES* snes_;  // Non-owning. SNES owns this SystemBus; pointer back to parent.
  uint8_t last_data_bus_value_ = 0xFF;
  BusEventSink* event_sink_ = nullptr;
  using PageRow = std::array<PageTableEntry, 256>;
  std::array<PageRow, 256> page_table_{};

  BusFollowResult FollowInline(const BusPlan& plan, TimeMasterT current_time);
  BusFollowResult FollowScheduled(const BusPlan& plan, TimeMasterT current_time, DeviceIdT source_device);
  // Out-of-line helper so the fast-path TryFast* callers stay small. Caller
  // must pre-check event_sink_ for null.
  void NotifyEvent(BusEventKind kind, SnesAddrT address, uint8_t data);
  [[nodiscard]] DebugReadResult MakeDebugReadResult(const BusPlan& plan) const;
  [[nodiscard]] DebugWriteResult MakeDebugWriteResult(const BusPlan& plan, uint8_t value) const;

  friend struct SystemBusTestAccess;
};

}  // namespace pupsnes
