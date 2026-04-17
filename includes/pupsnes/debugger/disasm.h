#pragma once

#include <array>
#include <cstddef>
#include <string>

#include "pupsnes/5a22/opcode_metadata.h"
#include "pupsnes/hw/snes.h"

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

[[nodiscard]] DisassembledInstruction DisassembleInstruction(const SNES& snes, SnesAddrT pc, const CpuFlags& flags);

}  // namespace pupsnes::debugger
