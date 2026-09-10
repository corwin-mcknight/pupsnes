#pragma once

#include <cstdint>

namespace pupsnes {

class Spc700Bus {
 public:
  virtual ~Spc700Bus() = default;
  virtual uint8_t Read(uint16_t address) = 0;
  virtual void Write(uint16_t address, uint8_t value) = 0;
};

// SPC700 execution core with all 256 opcodes.
// Each call advances one SPC cycle, including idle and dummy-read cycles. Bus
// effects occur only on that cycle, so execution may pause between port accesses.
// SLEEP/STOP retain their read/idle bus loop until reset; the SNES S-SMP has no
// connected interrupt input to wake SLEEP in this integration.
class Spc700 {
 public:
  struct State {
    uint8_t a = 0;
    uint8_t x = 0;
    uint8_t y = 0;
    uint8_t sp = 0;
    uint8_t psw = 0;
    uint16_t pc = 0xFFC0;
    uint64_t cycles = 0;
    bool stopped = false;
    bool sleeping = false;
    bool faulted = false;
    uint8_t fault_opcode = 0;
    uint16_t fault_pc = 0;
  };

  explicit Spc700(Spc700Bus& bus);

  // Deterministic power-on seed, not a claim about unspecified hardware register
  // contents. The IPL initializes the registers it needs and establishes SP.
  void Reset();
  // Initializes registers at an instruction boundary and discards any pending
  // instruction. This is not a save-state restore API for mid-instruction state.
  void Reset(const State& state);
  void TickCycle();

  [[nodiscard]] const State& GetState() const { return state_; }

 private:
  [[nodiscard]] uint8_t Fetch();
  [[nodiscard]] uint16_t DirectAddress(uint8_t offset) const;
  void SetNZ(uint8_t value);
  void SetNZWord(uint16_t value);
  void Compare(uint8_t lhs, uint8_t rhs);
  [[nodiscard]] uint8_t AddWithCarry(uint8_t lhs, uint8_t rhs);
  [[nodiscard]] uint8_t BinaryResult(uint8_t lhs, uint8_t rhs);
  [[nodiscard]] uint8_t ModifyResult(uint8_t value);
  void ArithmeticWord(uint16_t rhs);
  void Push(uint8_t value);
  [[nodiscard]] uint8_t Pull();
  void BranchRelative();

  Spc700Bus& bus_;
  State state_{};
  uint8_t opcode_ = 0;
  uint8_t instruction_cycle_ = 0;
  uint8_t instruction_cycles_ = 0;
  uint8_t operand_ = 0;
  uint8_t data_ = 0;
  uint16_t address_ = 0;
};

}  // namespace pupsnes
