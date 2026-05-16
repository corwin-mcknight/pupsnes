#pragma once

#include <cstddef>
#include <vector>

#include "pupsnes/debugger/ring_buffer.h"
#include "pupsnes/core/debugger_contract.h"

namespace pupsnes::debugger {

// Re-export the core TraceEntry so existing debugger code keeps compiling with
// the shorter name.
using pupsnes::TraceEntry;

class TraceLog : public TraceSink {
 public:
  explicit TraceLog(std::size_t capacity = 256) : ring_(capacity) {}

  void Push(const TraceEntry& entry) { ring_.Push(entry); }
  void Record(const TraceEntry& entry) override { Push(entry); }
  void Clear() { ring_.Clear(); }
  [[nodiscard]] std::vector<TraceEntry> Snapshot() const { return ring_.Snapshot(); }
  [[nodiscard]] std::size_t Size() const { return ring_.Size(); }

 private:
  RingBuffer<TraceEntry> ring_;
};

}  // namespace pupsnes::debugger
