#pragma once

// Test-private oracle derived from Bruce Clark's 65C816 reference
// (docs/plans/6502opcodes.md). Every implemented opcode gets one SpecEntry
// row with its canonical mnemonic, addressing mode, length formula, cycle
// formula, and touched-flag bitmask. Sweep tests in cpu_opcode_defs_tests.cpp
// cross-check the lowered opcode table against these rows so drift between
// the spec and the implementation fails compilation or tests.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pupsnes::cpu_spec_oracle {

// Flag bitmask: one bit per flag letter in Bruce Clark's "nvmxdizc" column.
// A bit is set iff the spec lists the corresponding column as '*', 'm', or 'x'
// (i.e. the flag is touched by the instruction). Bits with '.' are not
// touched and must be preserved.
enum FlagBit : uint8_t {
  kFlagC = 1U << 0U,
  kFlagZ = 1U << 1U,
  kFlagI = 1U << 2U,
  kFlagD = 1U << 3U,
  kFlagX = 1U << 4U,
  kFlagM = 1U << 5U,
  kFlagV = 1U << 6U,
  kFlagN = 1U << 7U,
};

constexpr uint8_t FlagsFrom(std::string_view nvmxdizc) {
  // Column order "nvmxdizc". A '.' means "preserved".
  uint8_t mask = 0;
  if (nvmxdizc.size() != 8) {
    return 0;
  }
  if (nvmxdizc[0] != '.') mask |= kFlagN;
  if (nvmxdizc[1] != '.') mask |= kFlagV;
  if (nvmxdizc[2] != '.') mask |= kFlagM;
  if (nvmxdizc[3] != '.') mask |= kFlagX;
  if (nvmxdizc[4] != '.') mask |= kFlagD;
  if (nvmxdizc[5] != '.') mask |= kFlagI;
  if (nvmxdizc[6] != '.') mask |= kFlagZ;
  if (nvmxdizc[7] != '.') mask |= kFlagC;
  return mask;
}

struct FormulaInputs {
  uint8_t m = 1;  // 1 when 8-bit accumulator/memory, 0 when 16-bit
  uint8_t x = 1;  // 1 when 8-bit index, 0 when 16-bit
  uint8_t w = 0;  // direct-page low byte nonzero penalty
  uint8_t p = 0;  // page-crossed penalty flag
  uint8_t t = 0;  // branch-taken flag
  uint8_t e = 0;  // emulation mode
};

// Evaluate a Bruce Clark formula expressed as a flat string of tokens joined
// by '+', '-', or '*'. Only the token vocabulary used by the implemented
// opcodes is accepted: decimal integers, and the single-letter variables
// m/x/w/p/t/e. Multiplication binds tighter than +/- (standard precedence).
// Returns 0 on parse error; callers assert the formula is non-empty.
constexpr int EvalFormula(std::string_view formula, const FormulaInputs& in) {
  const char* s = formula.data();
  const char* end = s + formula.size();

  auto skip_ws = [&]() {
    while (s < end && (*s == ' ' || *s == '\t')) ++s;
  };

  auto read_term = [&]() -> int {
    skip_ws();
    if (s >= end) return 0;
    int value = 0;
    if (*s >= '0' && *s <= '9') {
      while (s < end && *s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        ++s;
      }
    } else {
      char c = *s++;
      switch (c) {
        case 'm':
          value = in.m;
          break;
        case 'x':
          value = in.x;
          break;
        case 'w':
          value = in.w;
          break;
        case 'p':
          value = in.p;
          break;
        case 't':
          value = in.t;
          break;
        case 'e':
          value = in.e;
          break;
        default:
          return 0;
      }
    }
    skip_ws();
    while (s < end && *s == '*') {
      ++s;
      skip_ws();
      int rhs = 0;
      if (s < end && *s >= '0' && *s <= '9') {
        while (s < end && *s >= '0' && *s <= '9') {
          rhs = rhs * 10 + (*s - '0');
          ++s;
        }
      } else if (s < end) {
        char c = *s++;
        switch (c) {
          case 'm':
            rhs = in.m;
            break;
          case 'x':
            rhs = in.x;
            break;
          case 'w':
            rhs = in.w;
            break;
          case 'p':
            rhs = in.p;
            break;
          case 't':
            rhs = in.t;
            break;
          case 'e':
            rhs = in.e;
            break;
          default:
            return 0;
        }
      }
      value *= rhs;
      skip_ws();
    }
    return value;
  };

  int total = read_term();
  while (s < end) {
    skip_ws();
    if (s >= end) break;
    char op = *s++;
    int rhs = read_term();
    if (op == '+') {
      total += rhs;
    } else if (op == '-') {
      total -= rhs;
    } else {
      return 0;
    }
  }
  return total;
}

struct SpecEntry {
  uint8_t opcode;
  std::string_view mnemonic;
  std::string_view addressing;      // canonical Clark column ("imm", "abs", "long", "rel", "impl", "rlng", "stk")
  std::string_view length_formula;  // e.g. "1", "3-m", "3-x"
  std::string_view cycle_formula;   // e.g. "3-m", "2+t+t*e*p", "5-m"
  uint8_t flags_touched;            // mask of FlagBit values
  // Bits OR'd into the packed TimingCondition word before looking up a slot's
  // truth table. Set for opcodes whose implementation forces a condition at
  // runtime that isn't modeled as a "mode input" (e.g. BRA always forces
  // BranchTaken=1; the spec formula "3+e*p" has no `t` variable, so the
  // sweep must ignore the FormulaInputs.t value and evaluate as if t=1).
  uint8_t forced_condition_bits = 0;
  // True if this opcode's bit-4 timing slot is gated on the direct-page
  // low-byte-nonzero penalty (w) rather than the branch page-cross (p). The
  // CPU aliases both onto the same truth-table bit (branches never touch DP
  // and vice versa), so the sweep must force p=0 when packing bits for DP
  // opcodes, otherwise the "p=1" test mode spuriously fires the DP penalty.
  bool dp_penalty_bit = false;
};

// Bruce Clark's canonical addressing-mode tokens for each internal
// addressing-mode label used by our opcode specs. Multiple internal labels
// may map to the same Clark column (e.g. "immediate index" and "immediate"
// both use "imm"). Used by the Layer A sweep to normalize addressing-mode
// comparisons.
constexpr std::string_view NormalizeAddressing(std::string_view internal) {
  if (internal == "implied") return "impl";
  if (internal == "immediate") return "imm";
  if (internal == "immediate index") return "imm";
  if (internal == "immediate byte") return "imm";
  if (internal == "absolute") return "abs";
  if (internal == "absolute long") return "long";
  if (internal == "relative") return "rel";
  if (internal == "relative long") return "rlng";
  if (internal == "direct page") return "dir";
  if (internal == "direct page indexed X") return "dir,X";
  if (internal == "direct page indexed Y") return "dir,Y";
  if (internal == "absolute indexed X") return "abs,X";
  if (internal == "absolute indexed Y") return "abs,Y";
  if (internal == "stack relative") return "stk,S";
  if (internal == "direct indirect") return "(dir)";
  if (internal == "direct indirect long") return "[dir]";
  return internal;
}

// One row per currently implemented opcode. Add rows as new opcodes land.
// Columns lifted verbatim from docs/plans/6502opcodes.md. Flag masks built
// with FlagsFrom() from Clark's nvmxdizc column.
constexpr std::array<SpecEntry, 109> kSpec = {{
    // Misc
    {0xEA, "NOP", "impl", "1", "2", FlagsFrom("........")},

    // Stack push/pull
    {0x48, "PHA", "impl", "1", "4-m", FlagsFrom("........")},
    {0x8B, "PHB", "impl", "1", "3", FlagsFrom("........")},
    {0xAB, "PLB", "impl", "1", "4", FlagsFrom("n.....z.")},
    {0xDA, "PHX", "impl", "1", "4-x", FlagsFrom("........")},
    {0x5A, "PHY", "impl", "1", "4-x", FlagsFrom("........")},
    {0x08, "PHP", "impl", "1", "3", FlagsFrom("........")},
    {0x0B, "PHD", "impl", "1", "4", FlagsFrom("........")},
    {0x4B, "PHK", "impl", "1", "3", FlagsFrom("........")},
    {0x68, "PLA", "impl", "1", "5-m", FlagsFrom("n.....z.")},
    {0xFA, "PLX", "impl", "1", "5-x", FlagsFrom("n.....z.")},
    {0x7A, "PLY", "impl", "1", "5-x", FlagsFrom("n.....z.")},
    {0x28, "PLP", "impl", "1", "4", FlagsFrom("nvmxdizc")},
    {0x2B, "PLD", "impl", "1", "5", FlagsFrom("n.....z.")},
    {0xF4, "PEA", "abs", "3", "5", FlagsFrom("........")},

    // Load immediate
    {0xA9, "LDA", "imm", "3-m", "3-m", FlagsFrom("n.....z.")},
    {0xA2, "LDX", "imm", "3-x", "3-x", FlagsFrom("n.....z.")},
    {0xA0, "LDY", "imm", "3-x", "3-x", FlagsFrom("n.....z.")},

    // Store absolute
    {0x8D, "STA", "abs", "3", "5-m", FlagsFrom("........")},
    {0x8E, "STX", "abs", "3", "5-x", FlagsFrom("........")},
    {0x8C, "STY", "abs", "3", "5-x", FlagsFrom("........")},
    {0x8F, "STA", "long", "4", "6-m", FlagsFrom("........")},
    {0x9C, "STZ", "abs", "3", "5-m", FlagsFrom("........")},
    {0x9E, "STZ", "abs,X", "3", "6-m", FlagsFrom("........")},
    {0x9D, "STA", "abs,X", "3", "6-m", FlagsFrom("........")},
    {0x99, "STA", "abs,Y", "3", "6-m", FlagsFrom("........")},

    // Stack relative
    {0xA3, "LDA", "stk,S", "2", "5-m", FlagsFrom("n.....z.")},
    {0x83, "STA", "stk,S", "2", "5-m", FlagsFrom("........")},

    // Direct indirect
    {0xB2, "LDA", "(dir)", "2", "6-m+w", FlagsFrom("n.....z."), 0U, true},
    {0x92, "STA", "(dir)", "2", "6-m+w", FlagsFrom("........"), 0U, true},
    {0xA7, "LDA", "[dir]", "2", "7-m+w", FlagsFrom("n.....z."), 0U, true},
    {0x87, "STA", "[dir]", "2", "7-m+w", FlagsFrom("........"), 0U, true},

    // Direct page
    {0xA5, "LDA", "dir", "2", "4-m+w", FlagsFrom("n.....z."), 0U, true},
    {0xA6, "LDX", "dir", "2", "4-x+w", FlagsFrom("n.....z."), 0U, true},
    {0xA4, "LDY", "dir", "2", "4-x+w", FlagsFrom("n.....z."), 0U, true},
    {0x85, "STA", "dir", "2", "4-m+w", FlagsFrom("........"), 0U, true},
    {0x86, "STX", "dir", "2", "4-x+w", FlagsFrom("........"), 0U, true},
    {0x84, "STY", "dir", "2", "4-x+w", FlagsFrom("........"), 0U, true},
    {0x64, "STZ", "dir", "2", "4-m+w", FlagsFrom("........"), 0U, true},

    // Direct page indexed
    {0xB5, "LDA", "dir,X", "2", "5-m+w", FlagsFrom("n.....z."), 0U, true},
    {0xB4, "LDY", "dir,X", "2", "5-x+w", FlagsFrom("n.....z."), 0U, true},
    {0xB6, "LDX", "dir,Y", "2", "5-x+w", FlagsFrom("n.....z."), 0U, true},
    {0x95, "STA", "dir,X", "2", "5-m+w", FlagsFrom("........"), 0U, true},
    {0x94, "STY", "dir,X", "2", "5-x+w", FlagsFrom("........"), 0U, true},
    {0x96, "STX", "dir,Y", "2", "5-x+w", FlagsFrom("........"), 0U, true},
    {0x74, "STZ", "dir,X", "2", "5-m+w", FlagsFrom("........"), 0U, true},

    // Inc/Dec registers
    {0x1A, "INC", "impl", "1", "2", FlagsFrom("n.....z.")},
    {0x3A, "DEC", "impl", "1", "2", FlagsFrom("n.....z.")},
    {0xE8, "INX", "impl", "1", "2", FlagsFrom("n.....z.")},
    {0xC8, "INY", "impl", "1", "2", FlagsFrom("n.....z.")},
    {0xCA, "DEX", "impl", "1", "2", FlagsFrom("n.....z.")},
    {0x88, "DEY", "impl", "1", "2", FlagsFrom("n.....z.")},

    // Flag set/clear
    {0x18, "CLC", "impl", "1", "2", FlagsFrom(".......c")},
    {0x38, "SEC", "impl", "1", "2", FlagsFrom(".......c")},
    {0x58, "CLI", "impl", "1", "2", FlagsFrom(".....i..")},
    {0x78, "SEI", "impl", "1", "2", FlagsFrom(".....i..")},
    {0xB8, "CLV", "impl", "1", "2", FlagsFrom(".v......")},
    {0xD8, "CLD", "impl", "1", "2", FlagsFrom("...d....")},
    {0xF8, "SED", "impl", "1", "2", FlagsFrom("...d....")},
    // XCE swaps carry and emulation; in our flag-touched vocabulary we track C
    // plus M and X (emulation forces both to 1, so they're observable in P).
    {0xFB, "XCE", "impl", "1", "2", FlagsFrom("..mx...c")},
    {0xC2, "REP", "imm", "2", "3", FlagsFrom("nvmxdizc")},
    {0xE2, "SEP", "imm", "2", "3", FlagsFrom("nvmxdizc")},

    // Transfers
    {0xAA, "TAX", "impl", "1", "2", FlagsFrom("n.....z.")},
    {0xA8, "TAY", "impl", "1", "2", FlagsFrom("n.....z.")},
    {0xBA, "TSX", "impl", "1", "2", FlagsFrom("n.....z.")},
    {0x8A, "TXA", "impl", "1", "2", FlagsFrom("n.....z.")},
    {0x9A, "TXS", "impl", "1", "2", FlagsFrom("........")},
    {0x9B, "TXY", "impl", "1", "2", FlagsFrom("n.....z.")},
    {0x98, "TYA", "impl", "1", "2", FlagsFrom("n.....z.")},
    {0xBB, "TYX", "impl", "1", "2", FlagsFrom("n.....z.")},
    {0x5B, "TCD", "impl", "1", "2", FlagsFrom("n.....z.")},
    {0x1B, "TCS", "impl", "1", "2", FlagsFrom("........")},
    {0x7B, "TDC", "impl", "1", "2", FlagsFrom("n.....z.")},
    {0x3B, "TSC", "impl", "1", "2", FlagsFrom("n.....z.")},

    // Branches. Clark lists conditional branches as "2+t+t*e*p"; the
    // unconditional BRA takes the branch unconditionally so its formula is
    // "3+e*p" (equivalent to the conditional form with t=1).
    {0x80, "BRA", "rel", "2", "3+e*p", FlagsFrom("........"), 1U << 0U},
    {0xD0, "BNE", "rel", "2", "2+t+t*e*p", FlagsFrom("........")},
    {0xF0, "BEQ", "rel", "2", "2+t+t*e*p", FlagsFrom("........")},
    {0x90, "BCC", "rel", "2", "2+t+t*e*p", FlagsFrom("........")},
    {0xB0, "BCS", "rel", "2", "2+t+t*e*p", FlagsFrom("........")},
    {0x10, "BPL", "rel", "2", "2+t+t*e*p", FlagsFrom("........")},
    {0x30, "BMI", "rel", "2", "2+t+t*e*p", FlagsFrom("........")},
    {0x50, "BVC", "rel", "2", "2+t+t*e*p", FlagsFrom("........")},
    {0x70, "BVS", "rel", "2", "2+t+t*e*p", FlagsFrom("........")},
    {0x82, "BRL", "rlng", "3", "4", FlagsFrom("........")},

    // ALU immediate
    {0x69, "ADC", "imm", "3-m", "3-m", FlagsFrom("nvm...mm")},
    {0xE9, "SBC", "imm", "3-m", "3-m", FlagsFrom("nvm...mm")},
    {0x29, "AND", "imm", "3-m", "3-m", FlagsFrom("n.m...m.")},
    {0x09, "ORA", "imm", "3-m", "3-m", FlagsFrom("n.m...m.")},
    {0x49, "EOR", "imm", "3-m", "3-m", FlagsFrom("n.m...m.")},
    {0xC9, "CMP", "imm", "3-m", "3-m", FlagsFrom("n.m...mm")},
    // BIT #imm only affects Z (Clark: "......m." — m column is the zero bit
    // because the z update is m-dependent; the N and V bits are preserved in
    // immediate form unlike BIT abs).
    {0x89, "BIT", "imm", "3-m", "3-m", FlagsFrom("......m.")},
    {0xE0, "CPX", "imm", "3-x", "3-x", FlagsFrom("n.x...xx")},
    {0xC0, "CPY", "imm", "3-x", "3-x", FlagsFrom("n.x...xx")},

    // Shift/rotate on accumulator
    {0x0A, "ASL", "impl", "1", "2", FlagsFrom("n.....zc")},
    {0x4A, "LSR", "impl", "1", "2", FlagsFrom("n.....zc")},
    {0x2A, "ROL", "impl", "1", "2", FlagsFrom("n.....zc")},
    {0x6A, "ROR", "impl", "1", "2", FlagsFrom("n.....zc")},

    // ALU direct page
    {0x65, "ADC", "dir", "2", "4-m+w", FlagsFrom("nvm...mm"), 0U, true},
    {0xE5, "SBC", "dir", "2", "4-m+w", FlagsFrom("nvm...mm"), 0U, true},
    {0x25, "AND", "dir", "2", "4-m+w", FlagsFrom("n.m...m."), 0U, true},
    {0x05, "ORA", "dir", "2", "4-m+w", FlagsFrom("n.m...m."), 0U, true},
    {0x45, "EOR", "dir", "2", "4-m+w", FlagsFrom("n.m...m."), 0U, true},
    {0xC5, "CMP", "dir", "2", "4-m+w", FlagsFrom("n.m...mm"), 0U, true},

    // Jumps
    {0x4C, "JMP", "abs", "3", "3", FlagsFrom("........")},
    {0x5C, "JMP", "long", "4", "4", FlagsFrom("........")},
    {0x20, "JSR", "abs", "3", "6", FlagsFrom("........")},
    {0x22, "JSL", "long", "4", "8", FlagsFrom("........")},
    {0x60, "RTS", "impl", "1", "6", FlagsFrom("........")},
    {0x6B, "RTL", "impl", "1", "6", FlagsFrom("........")},
}};

// Linear lookup — 256 entries max, table is tiny so we don't bother with a
// sorted/hashed index. Returns nullptr if the opcode isn't in the spec.
constexpr const SpecEntry* FindSpec(uint8_t opcode) {
  for (const SpecEntry& entry : kSpec) {
    if (entry.opcode == opcode) {
      return &entry;
    }
  }
  return nullptr;
}

// Count how many cycles the lowered execution entry will actually dispatch
// under a given condition-bit configuration. `packed_bits` follows the
// TimingCondition enum bit layout: bit0=branch_taken, bit1=acc16, bit2=idx16,
// bit3=emulation, bit4=branch_page_crossed. Includes the implicit opcode
// fetch cycle (+1). Used by the Layer A cycle-formula sweep.
template <typename Entry>
constexpr int CountActiveCycles(const Entry& entry, uint32_t packed_bits) {
  int count = 1;  // implicit opcode fetch
  const uint32_t bit_mask = 1U << packed_bits;
  for (uint8_t i = 0; i < entry.remaining_op_count; ++i) {
    const uint8_t rule_index = entry.ops[i].rule_index;
    if ((entry.rules[rule_index] & bit_mask) != 0U) {
      count += 1;
    }
  }
  return count;
}

// Encode a FormulaInputs into the packed TimingCondition bits. m=1/x=1 mean
// the *16-bit* condition bit is 0 (the implementation flag tracks the
// "wider" state). Branch conditions track t directly; page-cross/emulation
// map onto their respective bits.
constexpr uint32_t PackConditionBits(const FormulaInputs& in) {
  uint32_t bits = 0;
  if (in.t) bits |= 1U << 0U;   // kBranchTaken
  if (!in.m) bits |= 1U << 1U;  // kAccumulator16 (m=0 → 16-bit → bit set)
  if (!in.x) bits |= 1U << 2U;  // kIndex16
  if (in.e) bits |= 1U << 3U;   // kEmulationMode
  if (in.p) bits |= 1U << 4U;   // kBranchPageCrossed
  return bits;
}

}  // namespace pupsnes::cpu_spec_oracle
