#pragma once

#include <cstddef>
#include <vector>

#include "pupsnes/debugger/ring_buffer.h"
#include "pupsnes/hw/bus_event.h"

namespace pupsnes::debugger {

// Fixed-capacity ring buffer of bus transactions for the debugger bus viewer.
// Always-on: the recording cost (a null-check + ring-buffer store per access)
// is negligible compared to the surrounding Plan/Follow + device dispatch, so
// there is no user-facing arm/disarm toggle.
class BusEventLog : public BusEventSink {
 public:
  explicit BusEventLog(std::size_t capacity = 2048) : ring_(capacity) {}

  void OnBusEvent(const BusEvent& event) override { Push(event); }
  void Push(const BusEvent& event) { ring_.Push(event); }
  void Clear() { ring_.Clear(); }

  [[nodiscard]] std::size_t Size() const { return ring_.Size(); }
  [[nodiscard]] std::size_t Capacity() const { return ring_.Capacity(); }
  [[nodiscard]] std::vector<BusEvent> Snapshot() const { return ring_.Snapshot(); }

 private:
  RingBuffer<BusEvent> ring_;
};

}  // namespace pupsnes::debugger
