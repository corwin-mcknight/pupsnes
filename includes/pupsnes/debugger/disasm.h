#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>

#include "pupsnes/hw/5a22/opcode_metadata.h"
#include "pupsnes/core/snes.h"

namespace pupsnes::debugger {

struct DisassembledInstruction {
  SnesAddrT pc = 0;
  uint8_t opcode = 0;
  uint8_t length = 1;
  std::size_t byte_count = 0;
  std::array<uint8_t, 4> bytes{};
  std::string text;
  bool complete = true;
};

[[nodiscard]] DisassembledInstruction DisassembleInstructionRaw(const SNES& snes, SnesAddrT pc, const CpuFlags& flags);
[[nodiscard]] std::string FormatDisassembly(const DisassembledInstruction& raw);
[[nodiscard]] DisassembledInstruction DisassembleInstruction(const SNES& snes, SnesAddrT pc, const CpuFlags& flags);

// Returns the target address of a branch instruction (BPL/BMI/BVC/BVS/BRA/BCC/BCS/BNE/BEQ/BRL),
// or std::nullopt if the instruction is not a relative branch or its operand bytes are incomplete.
// The target stays within the same program bank as the source PC.
[[nodiscard]] std::optional<SnesAddrT> GetBranchTarget(const DisassembledInstruction& instr);

}  // namespace pupsnes::debugger
