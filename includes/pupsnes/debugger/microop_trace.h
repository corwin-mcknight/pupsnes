#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

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

  void OnInstructionBegin(uint8_t opcode, SnesAddrT pc) override;
  void OnMicroOp(const MicroOpRecord& rec) override;
  void OnInstructionEnd(uint64_t retired_seq) override;

  [[nodiscard]] const std::optional<InstructionTrace>& Current() const { return current_; }
  [[nodiscard]] std::size_t RetiredSize() const { return size_; }
  [[nodiscard]] const InstructionTrace* RetiredAt(std::size_t index) const;
  void Clear();

 private:
  std::optional<InstructionTrace> current_;
  std::array<InstructionTrace, kCapacity> ring_{};
  std::size_t head_ = 0;
  std::size_t size_ = 0;
};

}  // namespace pupsnes::debugger
