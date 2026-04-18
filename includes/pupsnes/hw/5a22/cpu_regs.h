#pragma once

#include <cstdint>

namespace pupsnes {

// 65C816 processor status register.
// E (emulation mode) is not stored in P but is tracked here alongside it.
struct CpuFlags {
  bool N = false;  // Negative
  bool V = false;  // Overflow
  bool M = true;   // Memory/accumulator select: 1=8-bit (always true in
                   // emulation mode)
  bool X = true;   // Index register select: 1=8-bit (always true in emulation mode)
  bool D = false;  // Decimal mode
  bool I = true;   // IRQ disable
  bool Z = false;  // Zero
  bool C = false;  // Carry
  bool E = true;   // Emulation mode (toggled via XCE; not a P register bit)

  [[nodiscard]] uint8_t ToByte() const;
  void FromByte(uint8_t p, bool emulation_mode);
};

struct CpuRegs {
  uint16_t A = 0;
  uint16_t X = 0;
  uint16_t Y = 0;
  uint16_t SP = 0x01FF;  // Top of page 1 in emulation mode
  uint16_t DP = 0;
  uint8_t PBR = 0;
  uint8_t DBR = 0;
  uint16_t PC = 0;
  CpuFlags P{};
};

}  // namespace pupsnes
