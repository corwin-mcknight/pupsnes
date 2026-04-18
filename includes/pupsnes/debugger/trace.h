#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "pupsnes/hw/5a22/cpu.h"

namespace pupsnes::debugger {

struct TraceEntry {
  TimeMasterT master_time = 0;
  SnesAddrT pc = 0;
  uint8_t opcode = 0;
  uint8_t length = 1;
  std::size_t byte_count = 0;
  std::array<uint8_t, 4> bytes{};
  bool complete = true;
  CPU::Regs regs{};
};

class TraceLog {
 public:
  explicit TraceLog(std::size_t capacity = 256) : capacity_(capacity) {}

  void Push(TraceEntry entry);
  void Clear();
  [[nodiscard]] std::vector<TraceEntry> Snapshot() const;
  [[nodiscard]] std::size_t Size() const { return entries_.size(); }

 private:
  std::size_t capacity_ = 256;
  std::deque<TraceEntry> entries_;
};

}  // namespace pupsnes::debugger
