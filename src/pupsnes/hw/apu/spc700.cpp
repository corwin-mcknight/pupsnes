#include "pupsnes/hw/apu/spc700.h"

#include <array>
#include <utility>

namespace pupsnes {
namespace {

constexpr uint8_t kCarry = 0x01;
constexpr uint8_t kZero = 0x02;
constexpr uint8_t kInterrupt = 0x04;
constexpr uint8_t kHalfCarry = 0x08;
constexpr uint8_t kBreak = 0x10;
constexpr uint8_t kDirectPage = 0x20;
constexpr uint8_t kOverflow = 0x40;
constexpr uint8_t kNegative = 0x80;

// Total cycles include the opcode fetch. Branch entries include their taken
// penalty; the condition-sampling cycle shortens an untaken branch by two.
// Rows are the high opcode nibble and columns the low nibble. Timing follows
// the ares SPC700 instruction bus schedules referenced in docs/apu.md.
constexpr std::array<uint8_t, 256> kInstructionCycles = {
    // 0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
    2, 8, 4, 7, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 6,  8,  // 0x
    4, 8, 4, 7, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 4,  6,  // 1x
    2, 8, 4, 7, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 7,  4,  // 2x
    4, 8, 4, 7, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 3,  8,  // 3x
    2, 8, 4, 7, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 6,  6,  // 4x
    4, 8, 4, 7, 4, 5, 5, 6, 5, 5, 4, 5, 2, 2, 4,  3,  // 5x
    2, 8, 4, 7, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 7,  5,  // 6x
    4, 8, 4, 7, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3,  6,  // 7x
    2, 8, 4, 7, 3, 4, 3, 6, 2, 6, 5, 4, 5, 2, 4,  5,  // 8x
    4, 8, 4, 7, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 12, 5,  // 9x
    3, 8, 4, 7, 3, 4, 3, 6, 2, 6, 4, 4, 5, 2, 4,  4,  // Ax
    4, 8, 4, 7, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3,  4,  // Bx
    3, 8, 4, 7, 4, 5, 4, 7, 2, 5, 6, 4, 5, 2, 4,  9,  // Cx
    4, 8, 4, 7, 5, 6, 6, 7, 4, 5, 5, 5, 2, 2, 8,  3,  // Dx
    2, 8, 4, 7, 3, 4, 3, 6, 2, 4, 5, 3, 4, 3, 4,  3,  // Ex
    4, 8, 4, 7, 4, 5, 5, 6, 3, 4, 5, 4, 2, 2, 6,  3,  // Fx
};

}  // namespace

Spc700::Spc700(Spc700Bus& bus) : bus_(bus) { Reset(); }

void Spc700::Reset() { Reset(State{}); }

void Spc700::Reset(const State& state) {
  state_ = state;
  opcode_ = 0;
  instruction_cycle_ = 0;
  instruction_cycles_ = 0;
  operand_ = 0;
  data_ = 0;
  address_ = 0;
}

uint8_t Spc700::Fetch() {
  const uint8_t value = bus_.Read(state_.pc);
  ++state_.pc;
  return value;
}

uint16_t Spc700::DirectAddress(uint8_t offset) const {
  return static_cast<uint16_t>(((state_.psw & kDirectPage) != 0 ? 0x0100U : 0U) | offset);
}

void Spc700::SetNZ(uint8_t value) {
  state_.psw =
      static_cast<uint8_t>((state_.psw & ~(kNegative | kZero)) | (value & kNegative) | (value == 0 ? kZero : 0));
}

void Spc700::SetNZWord(uint16_t value) {
  state_.psw = static_cast<uint8_t>((state_.psw & ~(kNegative | kZero)) | ((value >> 8U) & kNegative) |
                                    (value == 0 ? kZero : 0));
}

void Spc700::Compare(uint8_t lhs, uint8_t rhs) {
  state_.psw = static_cast<uint8_t>((state_.psw & ~kCarry) | (lhs >= rhs ? kCarry : 0));
  SetNZ(static_cast<uint8_t>(lhs - rhs));
}

uint8_t Spc700::AddWithCarry(uint8_t lhs, uint8_t rhs) {
  const int sum = lhs + rhs + (state_.psw & kCarry);
  const uint8_t result = static_cast<uint8_t>(sum);
  state_.psw = static_cast<uint8_t>((state_.psw & ~(kCarry | kHalfCarry | kOverflow)) | (sum > 0xFF ? kCarry : 0) |
                                    (((lhs ^ rhs ^ sum) & 0x10) != 0 ? kHalfCarry : 0) |
                                    ((~(lhs ^ rhs) & (lhs ^ result) & 0x80) != 0 ? kOverflow : 0));
  SetNZ(result);
  return result;
}

uint8_t Spc700::BinaryResult(uint8_t lhs, uint8_t rhs) {
  // The six A/memory binary families share an addressing-mode matrix.
  // CMP X/Y use Compare directly because their opcodes lie outside that matrix.
  switch (opcode_ & 0xE0) {
    case 0x00: lhs = static_cast<uint8_t>(lhs | rhs); break;
    case 0x20: lhs = static_cast<uint8_t>(lhs & rhs); break;
    case 0x40: lhs = static_cast<uint8_t>(lhs ^ rhs); break;
    case 0x60: Compare(lhs, rhs); return lhs;
    case 0x80: return AddWithCarry(lhs, rhs);
    case 0xA0: return AddWithCarry(lhs, static_cast<uint8_t>(~rhs));
    default: std::unreachable();
  }
  SetNZ(lhs);
  return lhs;
}

uint8_t Spc700::ModifyResult(uint8_t value) {
  if (opcode_ == 0x1D || opcode_ == 0x8B || opcode_ == 0x8C || opcode_ == 0x9B || opcode_ == 0x9C || opcode_ == 0xDC) {
    --value;
  } else if (opcode_ == 0x3D || opcode_ == 0xAB || opcode_ == 0xAC || opcode_ == 0xBB || opcode_ == 0xBC ||
             opcode_ == 0xFC) {
    ++value;
  } else {
    const uint8_t carry_in = state_.psw & kCarry;
    const uint8_t carry_out =
        (opcode_ & 0x40) == 0 ? static_cast<uint8_t>(value >> 7U) : static_cast<uint8_t>(value & kCarry);
    switch (opcode_ & 0x60) {
      case 0x00: value = static_cast<uint8_t>(value << 1U); break;                       // ASL
      case 0x20: value = static_cast<uint8_t>((value << 1U) | carry_in); break;          // ROL
      case 0x40: value = static_cast<uint8_t>(value >> 1U); break;                       // LSR
      case 0x60: value = static_cast<uint8_t>((value >> 1U) | (carry_in << 7U)); break;  // ROR
      default: std::unreachable();
    }
    state_.psw = static_cast<uint8_t>((state_.psw & ~kCarry) | carry_out);
  }
  SetNZ(value);
  return value;
}

void Spc700::ArithmeticWord(uint16_t rhs) {
  const uint16_t lhs = static_cast<uint16_t>((state_.y << 8U) | state_.a);
  if (opcode_ == 0x5A) {
    state_.psw = static_cast<uint8_t>((state_.psw & ~kCarry) | (lhs >= rhs ? kCarry : 0));
    SetNZWord(static_cast<uint16_t>(lhs - rhs));
    return;
  }

  // ADDW/SUBW ignore incoming C. The byte chain derives H from bit 11 and
  // V from bit 15; only Z must then be replaced with a whole-word result.
  const bool subtract = opcode_ == 0x9A;
  state_.psw = static_cast<uint8_t>((state_.psw & ~kCarry) | (subtract ? kCarry : 0));
  if (subtract) {
    rhs = static_cast<uint16_t>(~rhs);
  }
  state_.a = AddWithCarry(state_.a, static_cast<uint8_t>(rhs));
  state_.y = AddWithCarry(state_.y, static_cast<uint8_t>(rhs >> 8U));
  SetNZWord(static_cast<uint16_t>((state_.y << 8U) | state_.a));
}

void Spc700::Push(uint8_t value) {
  bus_.Write(static_cast<uint16_t>(0x0100U | state_.sp), value);
  --state_.sp;
}

uint8_t Spc700::Pull() {
  ++state_.sp;
  return bus_.Read(static_cast<uint16_t>(0x0100U | state_.sp));
}

void Spc700::BranchRelative() {
  const int displacement = operand_ < 0x80 ? operand_ : static_cast<int>(operand_) - 0x100;
  state_.pc = static_cast<uint16_t>(state_.pc + displacement);
}

void Spc700::TickCycle() {
  ++state_.cycles;
  if (state_.faulted) {
    return;
  }
  if (state_.stopped || state_.sleeping) {
    // After the initial three-cycle halt instruction, continue its hardware
    // read/idle loop without fetching or executing another opcode.
    if (instruction_cycle_ == 0) {
      bus_.Read(state_.pc);
    }
    instruction_cycle_ ^= 1;
    return;
  }

  if (instruction_cycle_ == 0) {
    const uint16_t opcode_pc = state_.pc;
    opcode_ = Fetch();
    instruction_cycles_ = kInstructionCycles[opcode_];
    if (instruction_cycles_ == 0) {
      state_.faulted = true;
      state_.fault_opcode = opcode_;
      state_.fault_pc = opcode_pc;
      return;
    }
    instruction_cycle_ = 1;
    return;
  }

  ++instruction_cycle_;
  switch (opcode_) {
    case 0x00:
      bus_.Read(state_.pc);  // Implied instructions read without advancing PC.
      break;
    case 0x8D:
    case 0xCD:
    case 0xE8:
      data_ = Fetch();
      (opcode_ == 0x8D ? state_.y : (opcode_ == 0xCD ? state_.x : state_.a)) = data_;
      SetNZ(data_);
      break;
    case 0x68:
    case 0xAD:
    case 0xC8: Compare(opcode_ == 0x68 ? state_.a : (opcode_ == 0xAD ? state_.y : state_.x), Fetch()); break;
    case 0x08:
    case 0x28:
    case 0x48:
    case 0x88:
    case 0xA8: state_.a = BinaryResult(state_.a, Fetch()); break;
    case 0x1C:
    case 0x1D:
    case 0x3C:
    case 0x3D:
    case 0x5C:
    case 0x7C:
    case 0x9C:
    case 0xBC:
    case 0xDC:
    case 0xFC: {
      bus_.Read(state_.pc);
      auto& target =
          opcode_ == 0x1D || opcode_ == 0x3D ? state_.x : (opcode_ == 0xDC || opcode_ == 0xFC ? state_.y : state_.a);
      target = ModifyResult(target);
      break;
    }
    case 0x5D:
    case 0x7D:
    case 0x9D:
    case 0xBD:
    case 0xDD:
    case 0xFD:
      bus_.Read(state_.pc);
      if (opcode_ == 0xBD) {
        state_.sp = state_.x;  // This transfer alone preserves N/Z.
      } else if (opcode_ == 0x7D || opcode_ == 0xDD) {
        state_.a = opcode_ == 0x7D ? state_.x : state_.y;
        SetNZ(state_.a);
      } else if (opcode_ == 0x5D || opcode_ == 0x9D) {
        state_.x = opcode_ == 0x5D ? state_.a : state_.sp;
        SetNZ(state_.x);
      } else {
        state_.y = state_.a;
        SetNZ(state_.y);
      }
      break;
    case 0x20:
    case 0x40:
    case 0x60:
    case 0x80:
    case 0xA0:
    case 0xC0:
      if (instruction_cycle_ == 2) {
        bus_.Read(state_.pc);
      }
      if (instruction_cycle_ == instruction_cycles_) {
        const uint8_t mask = opcode_ == 0x20 || opcode_ == 0x40   ? kDirectPage
                             : opcode_ == 0x60 || opcode_ == 0x80 ? kCarry
                                                                  : kInterrupt;
        const bool set = opcode_ == 0x40 || opcode_ == 0x80 || opcode_ == 0xA0;
        state_.psw = static_cast<uint8_t>((state_.psw & ~mask) | (set ? mask : 0));
      }
      break;
    case 0x9F:
    case 0xE0:
    case 0xED:
      if (instruction_cycle_ == 2) {
        bus_.Read(state_.pc);
      }
      if (instruction_cycle_ == instruction_cycles_) {
        if (opcode_ == 0x9F) {
          state_.a = static_cast<uint8_t>((state_.a << 4U) | (state_.a >> 4U));
          SetNZ(state_.a);
        } else if (opcode_ == 0xE0) {
          state_.psw = static_cast<uint8_t>(state_.psw & ~(kOverflow | kHalfCarry));
        } else {
          state_.psw ^= kCarry;
        }
      }
      break;
    case 0x10:
    case 0x2F:
    case 0x30:
    case 0x50:
    case 0x70:
    case 0x90:
    case 0xB0:
    case 0xD0:
    case 0xF0:
      if (instruction_cycle_ == 2) {
        operand_ = Fetch();
        constexpr std::array<uint8_t, 4> kBranchFlags{kNegative, kOverflow, kCarry, kZero};
        const bool set = (state_.psw & kBranchFlags[opcode_ >> 6U]) != 0;
        const bool take = opcode_ == 0x2F || set == ((opcode_ & 0x20) != 0);
        if (!take) {
          instruction_cycles_ = 2;
        }
      } else if (instruction_cycle_ == 4) {
        BranchRelative();
      }
      break;
    case 0x03:
    case 0x13:
    case 0x23:
    case 0x33:
    case 0x43:
    case 0x53:
    case 0x63:
    case 0x73:
    case 0x83:
    case 0x93:
    case 0xA3:
    case 0xB3:
    case 0xC3:
    case 0xD3:
    case 0xE3:
    case 0xF3:
      if (instruction_cycle_ == 2) {
        operand_ = Fetch();
      } else if (instruction_cycle_ == 3) {
        data_ = bus_.Read(DirectAddress(operand_));
      } else if (instruction_cycle_ == 5) {
        operand_ = Fetch();
        const bool set = (data_ & (1U << (opcode_ >> 5U))) != 0;
        if (set != ((opcode_ & 0x10) == 0)) {
          instruction_cycles_ = 5;
        }
      } else if (instruction_cycle_ == 7) {
        BranchRelative();
      }
      break;
    case 0x2E:
    case 0xDE:
      if (instruction_cycle_ == 2) {
        operand_ = Fetch();
      } else if (instruction_cycle_ == (opcode_ == 0x2E ? 3 : 4)) {
        const uint8_t offset = opcode_ == 0x2E ? operand_ : static_cast<uint8_t>(operand_ + state_.x);
        data_ = bus_.Read(DirectAddress(offset));
      } else if (instruction_cycle_ == (opcode_ == 0x2E ? 5 : 6)) {
        operand_ = Fetch();
        if (state_.a == data_) {
          instruction_cycles_ = instruction_cycle_;
        }
      } else if (instruction_cycle_ == instruction_cycles_) {
        BranchRelative();
      }
      break;
    case 0x6E:
      if (instruction_cycle_ == 2) {
        operand_ = Fetch();
      } else if (instruction_cycle_ == 3) {
        data_ = bus_.Read(DirectAddress(operand_));
      } else if (instruction_cycle_ == 4) {
        --data_;
        bus_.Write(DirectAddress(operand_), data_);
      } else if (instruction_cycle_ == 5) {
        operand_ = Fetch();
        if (data_ == 0) {
          instruction_cycles_ = 5;
        }
      } else if (instruction_cycle_ == 7) {
        BranchRelative();
      }
      break;
    case 0xFE:
      if (instruction_cycle_ == 2) {
        bus_.Read(state_.pc);
      } else if (instruction_cycle_ == 4) {
        operand_ = Fetch();
        if (--state_.y == 0) {
          instruction_cycles_ = 4;
        }
      } else if (instruction_cycle_ == 6) {
        BranchRelative();
      }
      break;
    case 0x04:
    case 0x24:
    case 0x3E:
    case 0x44:
    case 0x64:
    case 0x7E:
    case 0x84:
    case 0xA4:
    case 0xE4:
    case 0xEB:
    case 0xF8:
      if (instruction_cycle_ == 2) {
        operand_ = Fetch();
      } else {
        data_ = bus_.Read(DirectAddress(operand_));
        if (opcode_ == 0x3E || opcode_ == 0x7E) {
          Compare(opcode_ == 0x3E ? state_.x : state_.y, data_);
        } else if (opcode_ < 0xC0) {
          state_.a = BinaryResult(state_.a, data_);
        } else {
          (opcode_ == 0xE4 ? state_.a : (opcode_ == 0xEB ? state_.y : state_.x)) = data_;
          SetNZ(data_);
        }
      }
      break;
    case 0x0B:
    case 0x2B:
    case 0x4B:
    case 0x6B:
    case 0x8B:
    case 0xAB:
    case 0xC4:
    case 0xCB:
    case 0xD8:
      if (instruction_cycle_ == 2) {
        operand_ = Fetch();
      } else if (instruction_cycle_ == 3) {
        data_ = bus_.Read(DirectAddress(operand_));
      } else {
        if (opcode_ < 0xC0) {
          data_ = ModifyResult(data_);
        } else {
          data_ = opcode_ == 0xC4 ? state_.a : (opcode_ == 0xCB ? state_.y : state_.x);
        }
        bus_.Write(DirectAddress(operand_), data_);
      }
      break;
    case 0x06:
    case 0x26:
    case 0x46:
    case 0x66:
    case 0x86:
    case 0xA6:
    case 0xC6:
    case 0xE6:
      if (instruction_cycle_ == 2) {
        bus_.Read(state_.pc);
      } else if (instruction_cycle_ == 3) {
        data_ = bus_.Read(DirectAddress(state_.x));
        if (opcode_ == 0xE6) {
          state_.a = data_;
          SetNZ(data_);
        } else if (opcode_ != 0xC6) {
          state_.a = BinaryResult(state_.a, data_);
        }
      } else {
        bus_.Write(DirectAddress(state_.x), state_.a);
      }
      break;
    case 0xAF:
    case 0xBF:
      if (instruction_cycle_ == 2) {
        bus_.Read(state_.pc);
      } else if (opcode_ == 0xBF && instruction_cycle_ == 3) {
        state_.a = bus_.Read(DirectAddress(state_.x));
        ++state_.x;
      } else if (instruction_cycle_ == 4) {
        if (opcode_ == 0xAF) {
          // The post-increment store has an idle cycle, not a target read.
          bus_.Write(DirectAddress(state_.x), state_.a);
          ++state_.x;
        } else {
          // The post-increment load updates N/Z on its final idle cycle.
          SetNZ(state_.a);
        }
      }
      break;
    case 0x05:
    case 0x0C:
    case 0x1E:
    case 0x25:
    case 0x2C:
    case 0x45:
    case 0x4C:
    case 0x5E:
    case 0x65:
    case 0x6C:
    case 0x85:
    case 0x8C:
    case 0xA5:
    case 0xAC:
    case 0xC5:
    case 0xC9:
    case 0xCC:
    case 0xE5:
    case 0xE9:
    case 0xEC:
      if (instruction_cycle_ == 2) {
        address_ = Fetch();
      } else if (instruction_cycle_ == 3) {
        address_ = static_cast<uint16_t>(address_ | (Fetch() << 8U));
      } else if (instruction_cycle_ == 4) {
        data_ = bus_.Read(address_);
        if (opcode_ == 0x1E || opcode_ == 0x5E) {
          Compare(opcode_ == 0x1E ? state_.x : state_.y, data_);
        } else if ((opcode_ & 0x0F) == 5 && opcode_ < 0xC0) {
          state_.a = BinaryResult(state_.a, data_);
        } else if (opcode_ == 0xE5 || opcode_ == 0xE9 || opcode_ == 0xEC) {
          (opcode_ == 0xE5 ? state_.a : (opcode_ == 0xE9 ? state_.x : state_.y)) = data_;
          SetNZ(data_);
        }
      } else {
        if (opcode_ < 0xC0) {
          data_ = ModifyResult(data_);
        } else {
          data_ = opcode_ == 0xC5 ? state_.a : (opcode_ == 0xC9 ? state_.x : state_.y);
        }
        bus_.Write(address_, data_);
      }
      break;
    case 0x14:
    case 0x1B:
    case 0x34:
    case 0x3B:
    case 0x54:
    case 0x5B:
    case 0x74:
    case 0x7B:
    case 0x94:
    case 0x9B:
    case 0xB4:
    case 0xBB:
    case 0xD4:
    case 0xD9:
    case 0xDB:
    case 0xF4:
    case 0xF9:
    case 0xFB:
      if (instruction_cycle_ == 2) {
        operand_ = Fetch();
      } else if (instruction_cycle_ == 3) {
        const uint8_t index = opcode_ == 0xD9 || opcode_ == 0xF9 ? state_.y : state_.x;
        address_ = DirectAddress(static_cast<uint8_t>(operand_ + index));
      } else if (instruction_cycle_ == 4) {
        data_ = bus_.Read(address_);
        if ((opcode_ & 0x0F) == 4 && opcode_ < 0xC0) {
          state_.a = BinaryResult(state_.a, data_);
        } else if (opcode_ == 0xF4 || opcode_ == 0xF9 || opcode_ == 0xFB) {
          (opcode_ == 0xF4 ? state_.a : (opcode_ == 0xF9 ? state_.x : state_.y)) = data_;
          SetNZ(data_);
        }
      } else {
        if (opcode_ < 0xC0) {
          data_ = ModifyResult(data_);
        } else {
          data_ = opcode_ == 0xD4 ? state_.a : (opcode_ == 0xD9 ? state_.x : state_.y);
        }
        bus_.Write(address_, data_);
      }
      break;
    case 0x15:
    case 0x16:
    case 0x35:
    case 0x36:
    case 0x55:
    case 0x56:
    case 0x75:
    case 0x76:
    case 0x95:
    case 0x96:
    case 0xB5:
    case 0xB6:
    case 0xD5:
    case 0xD6:
    case 0xF5:
    case 0xF6:
      if (instruction_cycle_ == 2) {
        address_ = Fetch();
      } else if (instruction_cycle_ == 3) {
        address_ = static_cast<uint16_t>(address_ | (Fetch() << 8U));
      } else if (instruction_cycle_ == 4) {
        // All absolute-indexed forms in this group end in 5 (X) or 6 (Y).
        const uint8_t index = (opcode_ & 1) != 0 ? state_.x : state_.y;
        address_ = static_cast<uint16_t>(address_ + index);
      } else if (instruction_cycle_ == 5) {
        data_ = bus_.Read(address_);
        if (opcode_ == 0xF5 || opcode_ == 0xF6) {
          state_.a = data_;
          SetNZ(data_);
        } else if (opcode_ != 0xD5 && opcode_ != 0xD6) {
          state_.a = BinaryResult(state_.a, data_);
        }
      } else {
        bus_.Write(address_, state_.a);
      }
      break;
    case 0x07:
    case 0x17:
    case 0x27:
    case 0x37:
    case 0x47:
    case 0x57:
    case 0x67:
    case 0x77:
    case 0x87:
    case 0x97:
    case 0xA7:
    case 0xB7:
    case 0xC7:
    case 0xE7:
    case 0xF7:
      if (instruction_cycle_ == 2) {
        operand_ = Fetch();
      } else if (instruction_cycle_ == 3) {
        // Bit 4 distinguishes [dp+X] from [dp]+Y in this opcode matrix.
        if ((opcode_ & 0x10) == 0) {
          operand_ = static_cast<uint8_t>(operand_ + state_.x);
        }
      } else if (instruction_cycle_ == 4) {
        address_ = bus_.Read(DirectAddress(operand_));
      } else if (instruction_cycle_ == 5) {
        const uint8_t high = bus_.Read(DirectAddress(static_cast<uint8_t>(operand_ + 1)));
        address_ = static_cast<uint16_t>(address_ | (high << 8U));
        if ((opcode_ & 0x10) != 0) {
          address_ = static_cast<uint16_t>(address_ + state_.y);
        }
      } else if (instruction_cycle_ == 6) {
        data_ = bus_.Read(address_);
        if (opcode_ == 0xE7 || opcode_ == 0xF7) {
          state_.a = data_;
          SetNZ(data_);
        } else if (opcode_ != 0xC7) {
          state_.a = BinaryResult(state_.a, data_);
        }
      } else {
        bus_.Write(address_, state_.a);
      }
      break;
    case 0x09:
    case 0x29:
    case 0x49:
    case 0x69:
    case 0x89:
    case 0xA9:
    case 0xFA:
      if (instruction_cycle_ == 2 || instruction_cycle_ == 4) {
        operand_ = Fetch();
      } else if (instruction_cycle_ == 3) {
        data_ = bus_.Read(DirectAddress(operand_));
      } else if (instruction_cycle_ == 5 && opcode_ != 0xFA) {
        data_ = BinaryResult(bus_.Read(DirectAddress(operand_)), data_);
      } else if (opcode_ != 0x69) {
        // MOV skips the destination read and writes on cycle five. Binary
        // operations expose flags with that read and write on cycle six.
        bus_.Write(DirectAddress(operand_), data_);
      }
      break;
    case 0x19:
    case 0x39:
    case 0x59:
    case 0x79:
    case 0x99:
    case 0xB9:
      if (instruction_cycle_ == 2) {
        bus_.Read(state_.pc);
      } else if (instruction_cycle_ == 3) {
        data_ = bus_.Read(DirectAddress(state_.y));
      } else if (instruction_cycle_ == 4) {
        data_ = BinaryResult(bus_.Read(DirectAddress(state_.x)), data_);
      } else if (opcode_ != 0x79) {
        bus_.Write(DirectAddress(state_.x), data_);
      }
      break;
    case 0x02:
    case 0x12:
    case 0x22:
    case 0x32:
    case 0x42:
    case 0x52:
    case 0x62:
    case 0x72:
    case 0x82:
    case 0x92:
    case 0xA2:
    case 0xB2:
    case 0xC2:
    case 0xD2:
    case 0xE2:
    case 0xF2:
      if (instruction_cycle_ == 2) {
        operand_ = Fetch();
      } else if (instruction_cycle_ == 3) {
        data_ = bus_.Read(DirectAddress(operand_));
      } else {
        const uint8_t mask = static_cast<uint8_t>(1U << (opcode_ >> 5U));
        data_ = static_cast<uint8_t>((opcode_ & 0x10) == 0 ? data_ | mask : data_ & ~mask);
        bus_.Write(DirectAddress(operand_), data_);
      }
      break;
    case 0x0A:
    case 0x2A:
    case 0x4A:
    case 0x6A:
    case 0x8A:
    case 0xAA:
    case 0xCA:
    case 0xEA:
      if (instruction_cycle_ == 2) {
        address_ = Fetch();
      } else if (instruction_cycle_ == 3) {
        const uint8_t high = Fetch();
        operand_ = static_cast<uint8_t>(1U << (high >> 5U));
        address_ = static_cast<uint16_t>((address_ | (high << 8U)) & 0x1FFF);
      } else if (instruction_cycle_ == 4) {
        data_ = bus_.Read(address_);
        if (opcode_ == 0xAA) {
          state_.psw = static_cast<uint8_t>((state_.psw & ~kCarry) | ((data_ & operand_) != 0 ? kCarry : 0));
        } else if (opcode_ == 0x4A || opcode_ == 0x6A) {
          const bool bit = (data_ & operand_) != 0;
          const bool carry = (state_.psw & kCarry) != 0 && (opcode_ == 0x4A ? bit : !bit);
          state_.psw = static_cast<uint8_t>((state_.psw & ~kCarry) | (carry ? kCarry : 0));
        }
      } else if (instruction_cycle_ == 5 && opcode_ != 0xCA) {
        if (opcode_ == 0xEA) {
          bus_.Write(address_, static_cast<uint8_t>(data_ ^ operand_));
        } else {
          const bool bit = (data_ & operand_) != 0;
          const bool old_carry = (state_.psw & kCarry) != 0;
          const bool carry = opcode_ == 0x8A ? old_carry != bit : old_carry || (opcode_ == 0x0A ? bit : !bit);
          state_.psw = static_cast<uint8_t>((state_.psw & ~kCarry) | (carry ? kCarry : 0));
        }
      } else if (instruction_cycle_ == 6) {
        data_ = static_cast<uint8_t>((data_ & ~operand_) | ((state_.psw & kCarry) != 0 ? operand_ : 0));
        bus_.Write(address_, data_);
      }
      break;
    case 0x0E:
    case 0x4E:
      if (instruction_cycle_ == 2) {
        address_ = Fetch();
      } else if (instruction_cycle_ == 3) {
        address_ = static_cast<uint16_t>(address_ | (Fetch() << 8U));
      } else if (instruction_cycle_ == 4) {
        data_ = bus_.Read(address_);
        SetNZ(static_cast<uint8_t>(state_.a - data_));
      } else if (instruction_cycle_ == 5) {
        bus_.Read(address_);  // Retain the first read despite this second bus access.
      } else {
        const uint8_t value = static_cast<uint8_t>(opcode_ == 0x0E ? data_ | state_.a : data_ & ~state_.a);
        bus_.Write(address_, value);
      }
      break;
    case 0x0D:
    case 0x2D:
    case 0x4D:
    case 0x6D:
      if (instruction_cycle_ == 2) {
        bus_.Read(state_.pc);
      } else if (instruction_cycle_ == 3) {
        data_ = opcode_ == 0x0D ? state_.psw : (opcode_ == 0x2D ? state_.a : (opcode_ == 0x4D ? state_.x : state_.y));
        Push(data_);
      }
      break;
    case 0x8E:
    case 0xAE:
    case 0xCE:
    case 0xEE:
      if (instruction_cycle_ == 2) {
        bus_.Read(state_.pc);
      } else if (instruction_cycle_ == 4) {
        data_ = Pull();
        // Register pulls preserve flags; pulling PSW restores all eight bits.
        (opcode_ == 0x8E ? state_.psw : (opcode_ == 0xAE ? state_.a : (opcode_ == 0xCE ? state_.x : state_.y))) = data_;
      }
      break;
    case 0x18:
    case 0x38:
    case 0x58:
    case 0x78:
    case 0x8F:
    case 0x98:
    case 0xB8:
      if (instruction_cycle_ == 2) {
        data_ = Fetch();
      } else if (instruction_cycle_ == 3) {
        operand_ = Fetch();
      } else if (instruction_cycle_ == 4) {
        const uint8_t value = bus_.Read(DirectAddress(operand_));
        if (opcode_ == 0x78) {
          Compare(value, data_);
        } else if (opcode_ != 0x8F) {
          data_ = BinaryResult(value, data_);
        }
      } else if (opcode_ != 0x78) {
        bus_.Write(DirectAddress(operand_), data_);
      }
      break;
    case 0x5A:
    case 0x7A:
    case 0x9A:
    case 0xBA:
      if (instruction_cycle_ == 2) {
        operand_ = Fetch();
      } else if (instruction_cycle_ == 3) {
        data_ = bus_.Read(DirectAddress(operand_));
      } else if (instruction_cycle_ == instruction_cycles_) {
        const uint8_t high = bus_.Read(DirectAddress(static_cast<uint8_t>(operand_ + 1)));
        const uint16_t word = static_cast<uint16_t>((high << 8U) | data_);
        if (opcode_ == 0xBA) {
          state_.a = data_;
          state_.y = high;
          SetNZWord(word);
        } else {
          ArithmeticWord(word);
        }
      }
      break;
    case 0x1A:
    case 0x3A:
      if (instruction_cycle_ == 2) {
        operand_ = Fetch();
      } else if (instruction_cycle_ == 3) {
        const uint8_t low = bus_.Read(DirectAddress(operand_));
        address_ = static_cast<uint16_t>(low + (opcode_ == 0x3A ? 1 : -1));
      } else if (instruction_cycle_ == 4) {
        bus_.Write(DirectAddress(operand_), static_cast<uint8_t>(address_));
      } else if (instruction_cycle_ == 5) {
        const uint8_t high = bus_.Read(DirectAddress(static_cast<uint8_t>(operand_ + 1)));
        address_ = static_cast<uint16_t>(address_ + (high << 8U));
      } else {
        bus_.Write(DirectAddress(static_cast<uint8_t>(operand_ + 1)), static_cast<uint8_t>(address_ >> 8U));
        SetNZWord(address_);
      }
      break;
    case 0xDA:
      if (instruction_cycle_ == 2) {
        operand_ = Fetch();
      } else if (instruction_cycle_ == 3) {
        bus_.Read(DirectAddress(operand_));
      } else if (instruction_cycle_ == 4) {
        bus_.Write(DirectAddress(operand_), state_.a);
      } else {
        bus_.Write(DirectAddress(static_cast<uint8_t>(operand_ + 1)), state_.y);
      }
      break;
    case 0xD7:
      if (instruction_cycle_ == 2) {
        operand_ = Fetch();
      } else if (instruction_cycle_ == 3) {
        address_ = bus_.Read(DirectAddress(operand_));
      } else if (instruction_cycle_ == 4) {
        const uint8_t high = bus_.Read(DirectAddress(static_cast<uint8_t>(operand_ + 1)));
        address_ = static_cast<uint16_t>((address_ | (high << 8U)) + state_.y);
      } else if (instruction_cycle_ == 6) {
        bus_.Read(address_);
      } else if (instruction_cycle_ == 7) {
        bus_.Write(address_, state_.a);
      }
      break;
    case 0x5F:
      if (instruction_cycle_ == 2) {
        address_ = Fetch();
      } else {
        const uint8_t high = Fetch();
        state_.pc = static_cast<uint16_t>(address_ | (high << 8U));
      }
      break;
    case 0x3F:
      if (instruction_cycle_ == 2) {
        address_ = Fetch();
      } else if (instruction_cycle_ == 3) {
        address_ = static_cast<uint16_t>(address_ | (Fetch() << 8U));
      } else if (instruction_cycle_ == 5) {
        Push(static_cast<uint8_t>(state_.pc >> 8U));
      } else if (instruction_cycle_ == 6) {
        Push(static_cast<uint8_t>(state_.pc));
      } else if (instruction_cycle_ == 8) {
        state_.pc = address_;
      }
      break;
    case 0x4F:
      if (instruction_cycle_ == 2) {
        address_ = static_cast<uint16_t>(0xFF00U | Fetch());
      } else if (instruction_cycle_ == 4) {
        Push(static_cast<uint8_t>(state_.pc >> 8U));
      } else if (instruction_cycle_ == 5) {
        Push(static_cast<uint8_t>(state_.pc));
      } else if (instruction_cycle_ == 6) {
        state_.pc = address_;
      }
      break;
    case 0x01:
    case 0x11:
    case 0x21:
    case 0x31:
    case 0x41:
    case 0x51:
    case 0x61:
    case 0x71:
    case 0x81:
    case 0x91:
    case 0xA1:
    case 0xB1:
    case 0xC1:
    case 0xD1:
    case 0xE1:
    case 0xF1:
      if (instruction_cycle_ == 2) {
        bus_.Read(state_.pc);
      } else if (instruction_cycle_ == 4) {
        Push(static_cast<uint8_t>(state_.pc >> 8U));
      } else if (instruction_cycle_ == 5) {
        Push(static_cast<uint8_t>(state_.pc));
      } else if (instruction_cycle_ == 7) {
        address_ = static_cast<uint16_t>(0xFFDEU - 2U * (opcode_ >> 4U));
        data_ = bus_.Read(address_);
      } else if (instruction_cycle_ == 8) {
        const uint8_t high = bus_.Read(static_cast<uint16_t>(address_ + 1));
        state_.pc = static_cast<uint16_t>(data_ | (high << 8U));
      }
      break;
    case 0x0F:
      if (instruction_cycle_ == 2) {
        bus_.Read(state_.pc);
      } else if (instruction_cycle_ == 3) {
        Push(static_cast<uint8_t>(state_.pc >> 8U));
      } else if (instruction_cycle_ == 4) {
        Push(static_cast<uint8_t>(state_.pc));
      } else if (instruction_cycle_ == 5) {
        Push(state_.psw);  // Stack the old B/I bits before changing either flag.
      } else if (instruction_cycle_ == 7) {
        data_ = bus_.Read(0xFFDE);
      } else if (instruction_cycle_ == 8) {
        const uint8_t high = bus_.Read(0xFFDF);
        state_.pc = static_cast<uint16_t>(data_ | (high << 8U));
        state_.psw = static_cast<uint8_t>((state_.psw & ~kInterrupt) | kBreak);
      }
      break;
    case 0x6F:
    case 0x7F:
      if (instruction_cycle_ == 2) {
        bus_.Read(state_.pc);
      } else if (opcode_ == 0x7F && instruction_cycle_ == 4) {
        state_.psw = Pull();
      } else if (instruction_cycle_ == instruction_cycles_ - 1) {
        data_ = Pull();
      } else if (instruction_cycle_ == instruction_cycles_) {
        const uint8_t high = Pull();
        state_.pc = static_cast<uint16_t>(data_ | (high << 8U));
      }
      break;
    case 0x1F:
      if (instruction_cycle_ == 2) {
        address_ = Fetch();
      } else if (instruction_cycle_ == 3) {
        const uint8_t high = Fetch();
        address_ = static_cast<uint16_t>((address_ | (high << 8U)) + state_.x);
      } else if (instruction_cycle_ == 5) {
        data_ = bus_.Read(address_);
      } else if (instruction_cycle_ == 6) {
        const uint8_t high = bus_.Read(static_cast<uint16_t>(address_ + 1));
        state_.pc = static_cast<uint16_t>(data_ | (high << 8U));
      }
      break;
    case 0xCF:
      if (instruction_cycle_ == 2) {
        bus_.Read(state_.pc);
      } else if (instruction_cycle_ == 9) {
        const uint16_t product = static_cast<uint16_t>(state_.y * state_.a);
        state_.a = static_cast<uint8_t>(product);
        state_.y = static_cast<uint8_t>(product >> 8U);
        SetNZ(state_.y);  // MUL derives both N and Z from the high byte only.
      }
      break;
    case 0x9E:
      if (instruction_cycle_ == 2) {
        bus_.Read(state_.pc);
      } else if (instruction_cycle_ == 12) {
        const unsigned dividend = static_cast<unsigned>((state_.y << 8U) | state_.a);
        const unsigned divisor = state_.x;
        state_.psw = static_cast<uint8_t>((state_.psw & ~(kHalfCarry | kOverflow)) |
                                          ((state_.y & 0x0F) >= (state_.x & 0x0F) ? kHalfCarry : 0) |
                                          (state_.y >= state_.x ? kOverflow : 0));
        if (state_.y < 2U * divisor) {
          // This path also produces a nine-bit quotient, truncated into A.
          state_.a = static_cast<uint8_t>(dividend / divisor);
          state_.y = static_cast<uint8_t>(dividend % divisor);
        } else {
          // The silicon overflow algorithm uses 256-X, so X=0 never causes
          // a host division by zero and still produces its hardware result.
          const unsigned excess = dividend - (divisor << 9U);
          const unsigned complement = 0x100U - divisor;
          state_.a = static_cast<uint8_t>(0xFFU - excess / complement);
          state_.y = static_cast<uint8_t>(divisor + excess % complement);
        }
        SetNZ(state_.a);
      }
      break;
    case 0xBE:
    case 0xDF:
      if (instruction_cycle_ == 2) {
        bus_.Read(state_.pc);
      } else if (opcode_ == 0xDF) {
        if ((state_.psw & kCarry) != 0 || state_.a > 0x99) {
          state_.a = static_cast<uint8_t>(state_.a + 0x60);
          state_.psw |= kCarry;
        }
        if ((state_.psw & kHalfCarry) != 0 || (state_.a & 0x0F) > 9) {
          state_.a = static_cast<uint8_t>(state_.a + 6);
        }
        SetNZ(state_.a);
      } else {
        if ((state_.psw & kCarry) == 0 || state_.a > 0x99) {
          state_.a = static_cast<uint8_t>(state_.a - 0x60);
          state_.psw = static_cast<uint8_t>(state_.psw & ~kCarry);
        }
        if ((state_.psw & kHalfCarry) == 0 || (state_.a & 0x0F) > 9) {
          state_.a = static_cast<uint8_t>(state_.a - 6);
        }
        SetNZ(state_.a);
      }
      break;
    case 0xEF:
    case 0xFF:
      if (instruction_cycle_ == 2) {
        bus_.Read(state_.pc);
      } else {
        (opcode_ == 0xEF ? state_.sleeping : state_.stopped) = true;
      }
      break;
    default:
      // Every byte has an SPC700 instruction; keep the dispatch exhaustive.
      std::unreachable();
  }

  if (instruction_cycle_ == instruction_cycles_) {
    instruction_cycle_ = 0;
  }
}

}  // namespace pupsnes
