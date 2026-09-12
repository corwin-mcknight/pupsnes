#pragma once

// Internal CPU types shared between the execution core (cpu.cpp) and the
// build-time opcode table (cpu_opcodes.cpp + cpu_opcode_defs_internal.h).
// Keep these out of the public cpu.h header so that adding or changing a
// MicroInternalOp variant, a timing rule, or the InstructionEntry layout does
// not trigger a recompile of every translation unit that includes cpu.h.

#include <array>
#include <cstdint>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/5a22/micro_op.h"

namespace pupsnes {

enum class TimingCondition : uint8_t {
  kBranchTaken = 0,
  kAccumulator16 = 1,
  kIndex16 = 2,
  kEmulationMode = 3,
  kBranchPageCrossed = 4,
  // Shares bit 4 with kBranchPageCrossed. No opcode uses both because branch
  // instructions never touch the direct page and DP-addressed instructions
  // never branch. EvaluateTimingRule ORs the separate DP-low-nonzero and
  // branch-page-crossed state into this slot. Opcode validation rejects
  // sequences that combine a DP fragment with a branch/index-cross producer.
  kDirectPageLowNonzero = 4,
  // Also shares bit 4. Set by kSetAddrHighDbrAddIndex when an absolute-indexed
  // *read* crosses a 256-byte page boundary; OR'd into the bit-4 source in
  // EvaluateTimingRule. Safe to alias: absolute-indexed reads never branch and
  // never use direct-page addressing, so the three bit-4 sources are mutually
  // exclusive per opcode. Indirect-indexed DP reads currently pay their index
  // cycle unconditionally; a conditional crossing there needs a separate bit.
  kIndexedPageCrossed = 4,
};

// Bit index of each TimingCondition within the packed-condition word used to
// index a rule's precomputed 32-entry truth table.
inline constexpr uint8_t kTimingConditionCount = 5;

enum class TimingRuleOp : uint8_t {
  kAlways = 0,
  kCondition = 1,
  kNot = 2,
  kAllOf = 3,
  kAnyOf = 4,
};

struct TimingRuleNode {
  TimingRuleOp op = TimingRuleOp::kAlways;
  TimingCondition condition = TimingCondition::kBranchTaken;
  uint8_t lhs = 0;
  uint8_t rhs = 0;
};

inline constexpr uint8_t kMaxTimingRuleNodes = 7;

// Build-time rule AST. Lowered to a 32-entry truth table (one uint32_t)
// before being stored in InstructionEntry; this struct is never loaded on
// the CPU hot path.
struct TimingRuleExpr {
  uint8_t node_count = 0;
  uint8_t root_index = 0;
  std::array<TimingRuleNode, kMaxTimingRuleNodes> nodes{};
};

struct MicroOp {
  MicroBusAction bus_action = MicroBusAction::kNone;
  MicroInternalOp internal_op = MicroInternalOp::kNone;
  uint8_t params = 0;
  uint8_t rule_index = 0;
};
static_assert(sizeof(MicroOp) == 4, "MicroOp must stay at 4 bytes");

inline constexpr uint8_t kMaxInstructionRules = 4;

enum class InstructionDisposition : uint8_t {
  kImplemented = 0,
  kFaultUnimplemented = 1,
};

// Per-opcode micro-op sequence (the cycles that follow the initial opcode
// fetch). Total instruction cycles = 1 (opcode fetch) + remaining_op_count.
//
// `rules` holds precomputed 32-entry truth tables: bit k of rules[i] is set
// iff rule i evaluates true when the packed-condition bits == k. Hot-path
// evaluation is a shift-mask-and-test over the truth table — the build-time
// TimingRuleExpr AST is not carried here.
struct InstructionEntry {
  InstructionDisposition disposition = InstructionDisposition::kFaultUnimplemented;
  uint8_t remaining_op_count = 0;
  uint8_t rule_count = 0;
  // True for opcodes whose bit-4 timing slot gates on kDirectPageLowNonzero
  // rather than kBranchPageCrossed. Controls whether FetchOpcode seeds
  // timing_context_.dp_low_nonzero from (DP & 0xFF) or leaves it cleared.
  // Without this gate, branches executed with DP-low nonzero would spuriously
  // trigger the emulation-mode page-cross penalty.
  bool uses_dp_penalty = false;
  // True for "new" 65C816 instructions whose emulation-mode behavior diverges
  // from the "old" 6502 / 65C02 set. Covers the stack family
  // (JSL/RTL/PEA/PEI/PER/PHB/PLB/PHD/PLD/PHK/JSR(abs,X)) which uses 16-bit SP
  // math during pushes/pulls (no wrap at $01xx) and restores the SP high byte
  // to $01 at end-of-instruction (Bruce Clark §2688). Also gates the
  // direct-page-indirect wrap suppression for PEI (§5.1.1: "PEI $FF does not
  // wrap at a page boundary, either the direct page part, or the (pushing
  // onto the) stack part"). PHX/PHY/PLX/PLY are NOT marked — they were
  // already on the 65C02 and use the old page-1 wrap.
  bool is_new_65816_instruction = false;
  std::array<MicroOp, kMaxRemainingOps> ops{};
  std::array<uint32_t, kMaxInstructionRules> rules{};
};
// 56 bytes with kMaxRemainingOps=8 and the two header flag bytes: 8 header
// (padded) + 32 ops + 16 rules. 256-entry table is 14 KiB, still L1i-resident.
static_assert(sizeof(InstructionEntry) <= 56, "InstructionEntry budget");

}  // namespace pupsnes
