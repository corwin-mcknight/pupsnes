#include "pupsnes/debugger/trace.h"

#include <algorithm>
#include <vector>

namespace pupsnes::debugger {

TraceLog::TraceLog(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) { ring_.resize(capacity_); }

void TraceLog::Push(const TraceEntry& entry) {
  ring_[write_count_ % capacity_] = entry;
  ++write_count_;
}

void TraceLog::Clear() { write_count_ = 0; }

std::size_t TraceLog::Size() const { return std::min(write_count_, capacity_); }

std::vector<TraceEntry> TraceLog::Snapshot() const {
  const std::size_t size = Size();
  std::vector<TraceEntry> out;
  out.reserve(size);
  const std::size_t start = (write_count_ >= capacity_) ? (write_count_ % capacity_) : 0;
  for (std::size_t i = 0; i < size; ++i) {
    out.push_back(ring_[(start + i) % capacity_]);
  }
  return out;
}

}  // namespace pupsnes::debugger
