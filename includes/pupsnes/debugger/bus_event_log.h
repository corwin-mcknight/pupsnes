#pragma once

#include <cstddef>
#include <vector>

#include "pupsnes/hw/bus_event.h"

namespace pupsnes::debugger {

// Fixed-capacity ring buffer of bus transactions for the debugger bus viewer.
// Always-on: the recording cost (a null-check + ring-buffer store per access)
// is negligible compared to the surrounding Plan/Follow + device dispatch, so
// there is no user-facing arm/disarm toggle.
class BusEventLog : public BusEventSink {
 public:
  explicit BusEventLog(std::size_t capacity = 2048);

  void OnBusEvent(const BusEvent& event) override { Push(event); }
  void Push(const BusEvent& event);
  void Clear();

  [[nodiscard]] std::size_t Size() const;
  [[nodiscard]] std::size_t Capacity() const { return capacity_; }
  [[nodiscard]] std::vector<BusEvent> Snapshot() const;

 private:
  std::vector<BusEvent> ring_;
  std::size_t capacity_ = 0;
  std::size_t write_count_ = 0;
};

}  // namespace pupsnes::debugger
