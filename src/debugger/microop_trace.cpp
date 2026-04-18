#include "pupsnes/debugger/microop_trace.h"

#include "pupsnes/5a22/opcode_metadata.h"

namespace pupsnes::debugger {

void MicroOpTrace::OnInstructionBegin(uint8_t opcode, SnesAddrT pc) {
  InstructionTrace t;
  t.opcode = opcode;
  t.opcode_address = pc;
  t.mnemonic = GetOpcodeMetadata(opcode).mnemonic;
  current_ = t;
}

void MicroOpTrace::OnMicroOp(const MicroOpRecord& rec) {
  if (!current_.has_value()) {
    return;
  }
  if (current_->op_count >= current_->ops.size()) {
    return;
  }
  current_->ops[current_->op_count++] = rec;
}

void MicroOpTrace::OnInstructionEnd(uint64_t retired_seq) {
  if (!current_.has_value()) {
    return;
  }
  current_->retired_seq = retired_seq;
  current_->completed = true;
  ring_[head_] = *current_;
  head_ = (head_ + 1) % kCapacity;
  if (size_ < kCapacity) {
    ++size_;
  }
  current_.reset();
}

const InstructionTrace* MicroOpTrace::RetiredAt(std::size_t index) const {
  if (index >= size_) {
    return nullptr;
  }
  const std::size_t start = (head_ + kCapacity - size_) % kCapacity;
  return &ring_[(start + index) % kCapacity];
}

void MicroOpTrace::Clear() {
  current_.reset();
  head_ = 0;
  size_ = 0;
}

}  // namespace pupsnes::debugger
