#pragma once

#include <cstdint>

#include "pupsnes/types.h"

namespace pupsnes {

// Classification of a single bus transaction for the debugger bus viewer.
// Each kind encodes both the access direction (read/write) and the path the
// transaction took through SystemBus::Follow — useful for diagnosing hot
// regions, MMIO traffic, and cross-clock stalls.
enum class BusEventKind : uint8_t {
  kFastRead = 0,        // kMemory + fast_read_ptr; skipped virtual dispatch
  kFastWrite = 1,       // kMemory + fast_write_ptr; skipped virtual dispatch
  kInlineRead = 2,      // FollowInline via Device::ReadRegister or catch-up MMIO
  kInlineWrite = 3,     // FollowInline via Device::WriteRegister or catch-up MMIO
  kScheduledRead = 4,   // FollowScheduled — cross-clock MMIO, token-backed
  kScheduledWrite = 5,  // FollowScheduled — cross-clock MMIO, token-backed
  kRejectedRead = 6,    // Unmapped read; returned open-bus value
  kRejectedWrite = 7,   // Unmapped write; dropped
};

struct BusEvent {
  TimeMasterT master_time = 0;
  SnesAddrT address = 0;  // 24-bit full bus address
  uint8_t data = 0;       // Read value or byte written
  BusEventKind kind = BusEventKind::kFastRead;
};

// Recorder interface for bus transactions. SystemBus holds an optional
// pointer to one of these; when non-null it is notified after every access.
// Implementations must be cheap on the hot path (the fast-path read/write
// routines call through this indirectly) and thread-free — the emulator is
// single-threaded.
class BusEventSink {
 public:
  virtual ~BusEventSink() = default;
  virtual void OnBusEvent(const BusEvent& event) = 0;
};

}  // namespace pupsnes
