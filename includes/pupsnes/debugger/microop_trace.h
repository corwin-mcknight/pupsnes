#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "pupsnes/debugger/ring_buffer.h"
#include "pupsnes/hw/5a22/cpu.h"

namespace pupsnes::debugger {

struct InstructionTrace {
  uint64_t retired_seq = 0;
  SnesAddrT opcode_address = 0;
  uint8_t opcode = 0;
  std::string_view mnemonic{};
  uint8_t op_count = 0;
  std::array<MicroOpRecord, kMaxRemainingOps + 1> ops{};
  bool completed = false;
};

class MicroOpTrace : public MicroOpRecorder {
 public:
  static constexpr std::size_t kCapacity = 256;

  MicroOpTrace() : retired_(kCapacity) {}

  void OnInstructionBegin(uint8_t opcode, SnesAddrT pc) override;
  void OnMicroOp(const MicroOpRecord& rec) override;
  void OnInstructionEnd(uint64_t retired_seq) override;

  [[nodiscard]] const std::optional<InstructionTrace>& Current() const { return current_; }
  [[nodiscard]] std::size_t RetiredSize() const { return retired_.Size(); }
  [[nodiscard]] const InstructionTrace* RetiredAt(std::size_t index) const {
    return index < retired_.Size() ? &retired_.At(index) : nullptr;
  }
  void Clear() {
    current_.reset();
    retired_.Clear();
  }

 private:
  std::optional<InstructionTrace> current_;
  RingBuffer<InstructionTrace> retired_;
};

}  // namespace pupsnes::debugger
