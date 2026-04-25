#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace pupsnes::debugger {

// Fixed-capacity ring over std::vector. Push is amortized O(1) with no
// per-call allocation — backing storage is sized once at construction, slots
// are overwritten in place, and a monotonic write counter drives head/tail
// math. Used by TraceLog, BusEventLog, and MicroOpTrace.
template <typename T>
class RingBuffer {
 public:
  explicit RingBuffer(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) { ring_.resize(capacity_); }

  void Push(const T& value) {
    ring_[write_count_ % capacity_] = value;
    ++write_count_;
  }

  void Clear() { write_count_ = 0; }

  [[nodiscard]] std::size_t Size() const { return std::min(write_count_, capacity_); }
  [[nodiscard]] std::size_t Capacity() const { return capacity_; }

  [[nodiscard]] const T& At(std::size_t index) const { return ring_[(StartIndex() + index) % capacity_]; }

  [[nodiscard]] std::vector<T> Snapshot() const {
    const std::size_t size = Size();
    std::vector<T> out;
    out.reserve(size);
    const std::size_t start = StartIndex();
    for (std::size_t i = 0; i < size; ++i) {
      out.push_back(ring_[(start + i) % capacity_]);
    }
    return out;
  }

 private:
  [[nodiscard]] std::size_t StartIndex() const { return (write_count_ >= capacity_) ? (write_count_ % capacity_) : 0; }

  std::vector<T> ring_;
  std::size_t capacity_ = 0;
  std::size_t write_count_ = 0;
};

}  // namespace pupsnes::debugger
