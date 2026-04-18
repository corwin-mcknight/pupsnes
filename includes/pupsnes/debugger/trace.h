#pragma once

#include <cstddef>
#include <vector>

#include "pupsnes/hw/debugger_contract.h"

namespace pupsnes::debugger {

// Re-export the core TraceEntry so existing debugger code keeps compiling with
// the shorter name.
using pupsnes::TraceEntry;

// Fixed-capacity ring over std::vector. Push is amortized O(1) with no
// per-call allocation — the backing storage is sized once at construction,
// slots are overwritten in place, and a monotonic write counter drives the
// head-tail math.
class TraceLog : public TraceSink {
 public:
  explicit TraceLog(std::size_t capacity = 256);

  void Push(const TraceEntry& entry);
  void Record(const TraceEntry& entry) override { Push(entry); }
  void Clear();
  [[nodiscard]] std::vector<TraceEntry> Snapshot() const;
  [[nodiscard]] std::size_t Size() const;

 private:
  std::vector<TraceEntry> ring_;
  std::size_t capacity_ = 0;
  std::size_t write_count_ = 0;  // Total pushes; size = min(write_count_, capacity_).
};

}  // namespace pupsnes::debugger
