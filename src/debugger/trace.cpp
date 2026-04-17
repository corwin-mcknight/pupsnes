#include "pupsnes/debugger/trace.h"

#include <utility>
#include <vector>

namespace pupsnes::debugger {

void TraceLog::Push(TraceEntry entry) {
  entries_.push_back(std::move(entry));
  while (entries_.size() > capacity_) {
    entries_.pop_front();
  }
}

void TraceLog::Clear() { entries_.clear(); }

std::vector<TraceEntry> TraceLog::Snapshot() const { return {entries_.begin(), entries_.end()}; }

}  // namespace pupsnes::debugger
