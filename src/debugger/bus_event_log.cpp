#include "pupsnes/debugger/bus_event_log.h"

#include <algorithm>
#include <vector>

namespace pupsnes::debugger {

BusEventLog::BusEventLog(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) { ring_.resize(capacity_); }

void BusEventLog::Push(const BusEvent& event) {
  ring_[write_count_ % capacity_] = event;
  ++write_count_;
}

void BusEventLog::Clear() { write_count_ = 0; }

std::size_t BusEventLog::Size() const { return std::min(write_count_, capacity_); }

std::vector<BusEvent> BusEventLog::Snapshot() const {
  const std::size_t size = Size();
  std::vector<BusEvent> out;
  out.reserve(size);
  const std::size_t start = (write_count_ >= capacity_) ? (write_count_ % capacity_) : 0;
  for (std::size_t i = 0; i < size; ++i) {
    out.push_back(ring_[(start + i) % capacity_]);
  }
  return out;
}

}  // namespace pupsnes::debugger
