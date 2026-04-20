#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "pupsnes/5a22/cpu_internal.h"
#include "pupsnes/hw/5a22/cpu.h"

namespace pupsnes::opcode_defs_internal {

namespace micro_op_params {
// Packing helpers for MicroOp::params. Populated in subsequent refactor
// steps as each enum group collapses into a parameterized category.

// Register-to-register transfers (kTransferReg): src in low nibble, dst in high
// nibble. Both nibbles reference the Reg enum defined in cpu.h.
inline constexpr uint8_t PackTransfer(Reg src, Reg dst) {
  return static_cast<uint8_t>(static_cast<uint8_t>(src) | (static_cast<uint8_t>(dst) << 4U));
}
inline constexpr Reg UnpackTransferSrc(uint8_t params) {
  return static_cast<Reg>(params & 0x0FU);
}
inline constexpr Reg UnpackTransferDst(uint8_t params) {
  return static_cast<Reg>((params >> 4U) & 0x0FU);
}

// Inc/dec registers (kIncDecReg): bit 0 = decrement, bits [4:1] = Reg (A/X/Y only).
inline constexpr uint8_t PackIncDec(Reg reg, bool decrement) {
  return static_cast<uint8_t>((decrement ? 1U : 0U) | (static_cast<uint32_t>(reg) << 1U));
}
inline constexpr Reg UnpackIncDecReg(uint8_t params) {
  return static_cast<Reg>((params >> 1U) & 0x0FU);
}
inline constexpr bool UnpackIncDecDecrement(uint8_t params) {
  return (params & 0x01U) != 0U;
}

// Flag set/clear (kSetFlag): bit 0 = value (1 = set, 0 = clear), bits [4:1] = Flag.
// Only C, D, I, V are used by flag-op opcodes; other Flag values are unused.
inline constexpr uint8_t PackSetFlag(Flag flag, bool value) {
  return static_cast<uint8_t>((value ? 1U : 0U) | (static_cast<uint32_t>(flag) << 1U));
}
inline constexpr Flag UnpackSetFlagFlag(uint8_t params) {
  return static_cast<Flag>((params >> 1U) & 0x0FU);
}
inline constexpr bool UnpackSetFlagValue(uint8_t params) {
  return (params & 0x01U) != 0U;
}

// Branch-condition setter (kSetBranchTakenCond): bits [3:0] = BranchCond
// (kAlways, kZ/kNotZ, kC/kNotC, kN/kNotN, kV/kNotV). Dispatch lives in
// DispatchSetBranch in cpu.cpp.
inline constexpr uint8_t PackBranchCond(BranchCond cond) {
  return static_cast<uint8_t>(static_cast<uint32_t>(cond) & 0x0FU);
}
inline constexpr BranchCond UnpackBranchCond(uint8_t params) {
  return static_cast<BranchCond>(params & 0x0FU);
}

// Register load from fetch_data_ (kLoadReg): bits [3:0] = Reg (A/X/Y only),
// bits [5:4] = ByteSel (kLow = 0, kHigh = 1), bit 6 = update_nz. Dispatch
// lives in DispatchLoadReg in cpu.cpp. On kHigh, update_nz must be true
// (that's the only high-byte variant the concrete helpers implement); the
// builder enforces this.
inline constexpr uint8_t PackLoadReg(Reg reg, ByteSel byte_sel, bool update_nz) {
  return static_cast<uint8_t>((static_cast<uint32_t>(reg) & 0x0FU) |
                              ((static_cast<uint32_t>(byte_sel) & 0x03U) << 4U) |
                              ((update_nz ? 1U : 0U) << 6U));
}
inline constexpr Reg UnpackLoadRegReg(uint8_t params) {
  return static_cast<Reg>(params & 0x0FU);
}
inline constexpr ByteSel UnpackLoadRegByteSel(uint8_t params) {
  return static_cast<ByteSel>((params >> 4U) & 0x03U);
}
inline constexpr bool UnpackLoadRegNz(uint8_t params) {
  return (params & 0x40U) != 0U;
}

// Push bus action (kPushStack): bits [3:0] = PushSrc (15 variants — A8/AHigh,
// X8/XHigh, Y8/YHigh, Pcl/Pch/Pbr, Dbr, P, DpLow/DpHigh, AddrLow/AddrHigh).
// Dispatch lives in DispatchPushStackByte in cpu.cpp.
inline constexpr uint8_t PackPushStack(PushSrc src) {
  return static_cast<uint8_t>(static_cast<uint32_t>(src) & 0x0FU);
}
inline constexpr PushSrc UnpackPushStack(uint8_t params) {
  return static_cast<PushSrc>(params & 0x0FU);
}
}  // namespace micro_op_params

struct CycleSlotSpec {
  MicroBusAction bus_action = MicroBusAction::kNone;
  MicroInternalOp internal_op = MicroInternalOp::kNone;
  TimingRuleExpr rule{};
  std::string_view label{};
  uint8_t params = 0;
};

struct CycleFragment {
  uint8_t cycle_count = 0;
  bool overflowed = false;
  std::array<CycleSlotSpec, kMaxRemainingOps> cycles{};
};

enum class OpcodeSpecDisposition : uint8_t {
  kImplemented = 0,
  kUnimplemented = 1,
};

struct OpcodeSpec {
  uint8_t opcode = 0;
  OpcodeSpecDisposition disposition = OpcodeSpecDisposition::kUnimplemented;
  std::string_view mnemonic = "???";
  std::string_view addressing_mode{};
  uint8_t cycle_count = 0;
  bool overflowed = false;
  std::array<CycleSlotSpec, kMaxRemainingOps> cycles{};
};

struct OpcodeMetadata {
  bool implemented = false;
  std::string_view mnemonic = "???";
  std::string_view addressing_mode{};
  uint8_t cycle_count = 0;
  std::array<std::string_view, kMaxRemainingOps> cycle_labels{};
};

struct OpcodeArtifacts {
  std::array<InstructionEntry, 256> execution_table{};
  std::array<OpcodeMetadata, 256> metadata_table{};
};

constexpr TimingRuleExpr Always() {
  TimingRuleExpr expr{};
  expr.node_count = 1;
  expr.root_index = 0;
  expr.nodes[0].op = TimingRuleOp::kAlways;
  return expr;
}

constexpr TimingRuleExpr Condition(TimingCondition condition) {
  TimingRuleExpr expr{};
  expr.node_count = 1;
  expr.root_index = 0;
  expr.nodes[0].op = TimingRuleOp::kCondition;
  expr.nodes[0].condition = condition;
  return expr;
}

constexpr TimingRuleNode ShiftTimingRuleNode(const TimingRuleNode& node, uint8_t offset) {
  TimingRuleNode shifted = node;
  switch (node.op) {
    case TimingRuleOp::kNot:
      shifted.lhs = static_cast<uint8_t>(node.lhs + offset);
      break;
    case TimingRuleOp::kAllOf:
    case TimingRuleOp::kAnyOf:
      shifted.lhs = static_cast<uint8_t>(node.lhs + offset);
      shifted.rhs = static_cast<uint8_t>(node.rhs + offset);
      break;
    case TimingRuleOp::kAlways:
    case TimingRuleOp::kCondition:
      break;
  }
  return shifted;
}

constexpr TimingRuleExpr Not(const TimingRuleExpr& expr) {
  TimingRuleExpr out{};
  out.node_count = static_cast<uint8_t>(expr.node_count + 1U);
  out.root_index = expr.node_count;

  const uint8_t copy_count = (expr.node_count < kMaxTimingRuleNodes) ? expr.node_count : kMaxTimingRuleNodes;
  for (uint8_t i = 0; i < copy_count; ++i) {
    out.nodes[i] = expr.nodes[i];
  }
  if (out.root_index < kMaxTimingRuleNodes) {
    out.nodes[out.root_index].op = TimingRuleOp::kNot;
    out.nodes[out.root_index].lhs = expr.root_index;
  }
  return out;
}

constexpr TimingRuleExpr MergeRules(TimingRuleOp op, const TimingRuleExpr& lhs, const TimingRuleExpr& rhs) {
  TimingRuleExpr out{};
  const uint8_t rhs_offset = lhs.node_count;
  out.node_count = static_cast<uint8_t>(lhs.node_count + rhs.node_count + 1U);
  out.root_index = static_cast<uint8_t>(lhs.node_count + rhs.node_count);

  for (uint8_t i = 0; i < lhs.node_count && i < kMaxTimingRuleNodes; ++i) {
    out.nodes[i] = lhs.nodes[i];
  }
  for (uint8_t i = 0; i < rhs.node_count && static_cast<uint8_t>(rhs_offset + i) < kMaxTimingRuleNodes; ++i) {
    out.nodes[rhs_offset + i] = ShiftTimingRuleNode(rhs.nodes[i], rhs_offset);
  }
  if (out.root_index < kMaxTimingRuleNodes) {
    out.nodes[out.root_index].op = op;
    out.nodes[out.root_index].lhs = lhs.root_index;
    out.nodes[out.root_index].rhs = static_cast<uint8_t>(rhs.root_index + rhs_offset);
  }
  return out;
}

constexpr TimingRuleExpr AllOf(const TimingRuleExpr& lhs, const TimingRuleExpr& rhs) {
  return MergeRules(TimingRuleOp::kAllOf, lhs, rhs);
}

constexpr TimingRuleExpr AnyOf(const TimingRuleExpr& lhs, const TimingRuleExpr& rhs) {
  return MergeRules(TimingRuleOp::kAnyOf, lhs, rhs);
}

constexpr CycleSlotSpec FetchPc(MicroInternalOp internal_op = MicroInternalOp::kNone, TimingRuleExpr rule = Always(),
                                std::string_view label = {}) {
  return CycleSlotSpec{MicroBusAction::kFetchPc, internal_op, rule, label};
}

constexpr CycleSlotSpec ReadAddr(MicroInternalOp internal_op = MicroInternalOp::kNone, TimingRuleExpr rule = Always(),
                                 std::string_view label = {}) {
  return CycleSlotSpec{MicroBusAction::kReadAddr, internal_op, rule, label};
}

constexpr CycleSlotSpec WriteA8Addr(MicroInternalOp internal_op = MicroInternalOp::kNone,
                                    TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{MicroBusAction::kWriteA8Addr, internal_op, rule, label};
}

constexpr CycleSlotSpec WriteX8Addr(MicroInternalOp internal_op = MicroInternalOp::kNone,
                                    TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{MicroBusAction::kWriteX8Addr, internal_op, rule, label};
}

constexpr CycleSlotSpec WriteY8Addr(MicroInternalOp internal_op = MicroInternalOp::kNone,
                                    TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{MicroBusAction::kWriteY8Addr, internal_op, rule, label};
}

constexpr CycleSlotSpec WriteAHighAddr(MicroInternalOp internal_op = MicroInternalOp::kNone,
                                       TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{MicroBusAction::kWriteAHighAddr, internal_op, rule, label};
}

constexpr CycleSlotSpec WriteXHighAddr(MicroInternalOp internal_op = MicroInternalOp::kNone,
                                       TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{MicroBusAction::kWriteXHighAddr, internal_op, rule, label};
}

constexpr CycleSlotSpec WriteYHighAddr(MicroInternalOp internal_op = MicroInternalOp::kNone,
                                       TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{MicroBusAction::kWriteYHighAddr, internal_op, rule, label};
}

// Parameterized push to the stack. Packs PushSrc into CycleSlotSpec::params
// for the unified kPushStack bus action. Dispatch lives in DispatchPushStackByte
// in cpu.cpp. internal_op defaults to kNone so callers can opt in (typically
// kDecrementSp to sequence SP after the write) at the emit site.
constexpr CycleSlotSpec PushReg(PushSrc src, MicroInternalOp internal_op = MicroInternalOp::kNone,
                                TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kPushStack,
      internal_op,
      rule,
      label,
      micro_op_params::PackPushStack(src),
  };
}

constexpr CycleSlotSpec PullStack(MicroInternalOp internal_op = MicroInternalOp::kNone, TimingRuleExpr rule = Always(),
                                  std::string_view label = {}) {
  return CycleSlotSpec{MicroBusAction::kPullStack, internal_op, rule, label};
}

constexpr CycleSlotSpec Internal(MicroInternalOp internal_op = MicroInternalOp::kNone, TimingRuleExpr rule = Always(),
                                 std::string_view label = {}) {
  return CycleSlotSpec{MicroBusAction::kNone, internal_op, rule, label};
}

// Parameterized register-to-register transfer. Packs (src, dst) into
// CycleSlotSpec::params for the unified kTransferReg internal op. Width and
// flag semantics of each concrete pair live in DispatchTransferReg in cpu.cpp.
constexpr CycleSlotSpec TransferReg(Reg src, Reg dst, TimingRuleExpr rule = Always(),
                                    std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kNone,
      MicroInternalOp::kTransferReg,
      rule,
      label,
      micro_op_params::PackTransfer(src, dst),
  };
}

// Parameterized register inc/dec. Packs (reg, decrement) into
// CycleSlotSpec::params for the unified kIncDecReg internal op. Width and flag
// semantics for each register live in DispatchIncDecReg in cpu.cpp.
constexpr CycleSlotSpec IncDecReg(Reg reg, bool decrement, TimingRuleExpr rule = Always(),
                                  std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kNone,
      MicroInternalOp::kIncDecReg,
      rule,
      label,
      micro_op_params::PackIncDec(reg, decrement),
  };
}

// Parameterized flag set/clear. Packs (flag, value) into CycleSlotSpec::params
// for the unified kSetFlag internal op. Dispatch lives in DispatchSetFlag in
// cpu.cpp. Only C, D, I, V are emitted by real opcodes (CLC/SEC/CLI/SEI/CLV/
// CLD/SED).
constexpr CycleSlotSpec SetFlag(Flag flag, bool value, TimingRuleExpr rule = Always(),
                                std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kNone,
      MicroInternalOp::kSetFlag,
      rule,
      label,
      micro_op_params::PackSetFlag(flag, value),
  };
}

// Parameterized load of a register byte from fetch_data_ (FetchPc bus action).
// Packs (reg, byte_sel, update_nz) into CycleSlotSpec::params for the unified
// kLoadReg internal op. Dispatch lives in DispatchLoadReg in cpu.cpp. Used by
// LDA/LDX/LDY immediate and any other opcode that completes a register via a
// PC-side fetch.
constexpr CycleSlotSpec LoadRegFromFetch(Reg reg, ByteSel byte_sel, bool update_nz,
                                         TimingRuleExpr rule = Always(),
                                         std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kFetchPc,
      MicroInternalOp::kLoadReg,
      rule,
      label,
      micro_op_params::PackLoadReg(reg, byte_sel, update_nz),
  };
}

// Parameterized load of a register byte from a pre-incremented stack pull
// (kPreIncPullStack bus action). Same param encoding as LoadRegFromFetch;
// used by PLA/PLX/PLY.
constexpr CycleSlotSpec PullPreIncLoadReg(Reg reg, ByteSel byte_sel, bool update_nz,
                                          TimingRuleExpr rule = Always(),
                                          std::string_view label = {}) {
  return CycleSlotSpec{
      MicroBusAction::kPreIncPullStack,
      MicroInternalOp::kLoadReg,
      rule,
      label,
      micro_op_params::PackLoadReg(reg, byte_sel, update_nz),
  };
}

// Parameterized branch-displacement fetch. Reads the signed 8-bit displacement
// from PBR:PC (advancing PC) and evaluates the branch condition in the same
// cycle, setting timing_context_.branch_taken via DispatchSetBranch. Packs the
// BranchCond into CycleSlotSpec::params for the unified kSetBranchTakenCond
// internal op. Used by every conditional branch opcode via BranchSequence;
// BRL has its own long-form sequence.
constexpr CycleSlotSpec FetchPcBranchTest(BranchCond cond,
                                          std::string_view label = "fetch displacement") {
  return CycleSlotSpec{
      MicroBusAction::kFetchPc,
      MicroInternalOp::kSetBranchTakenCond,
      Always(),
      label,
      micro_op_params::PackBranchCond(cond),
  };
}

struct CycleFragmentBuilder {
  CycleFragment fragment{};

  constexpr CycleFragmentBuilder& Then(const CycleSlotSpec& cycle) {
    if (fragment.cycle_count >= kMaxRemainingOps) {
      fragment.overflowed = true;
      return *this;
    }
    fragment.cycles[fragment.cycle_count++] = cycle;
    return *this;
  }

  constexpr CycleFragment Build() const { return fragment; }
};

constexpr CycleFragmentBuilder Fragment() { return CycleFragmentBuilder{}; }

struct OpcodeSpecBuilder {
  OpcodeSpec spec{};

  constexpr OpcodeSpecBuilder(uint8_t opcode, std::string_view mnemonic, std::string_view addressing_mode) {
    spec.opcode = opcode;
    spec.disposition = OpcodeSpecDisposition::kImplemented;
    spec.mnemonic = mnemonic;
    spec.addressing_mode = addressing_mode;
  }

  constexpr OpcodeSpecBuilder& Then(const CycleSlotSpec& cycle) {
    if (spec.cycle_count >= kMaxRemainingOps) {
      spec.overflowed = true;
      return *this;
    }
    spec.cycles[spec.cycle_count++] = cycle;
    return *this;
  }

  constexpr OpcodeSpecBuilder& Then(const CycleFragment& fragment) {
    spec.overflowed = spec.overflowed || fragment.overflowed;
    for (uint8_t i = 0; i < fragment.cycle_count; ++i) {
      Then(fragment.cycles[i]);
    }
    return *this;
  }

  constexpr OpcodeSpec Build() const { return spec; }
};

constexpr OpcodeSpecBuilder Opcode(uint8_t opcode, std::string_view mnemonic, std::string_view addressing_mode) {
  return OpcodeSpecBuilder(opcode, mnemonic, addressing_mode);
}

template <typename T, std::size_t N, std::size_t M>
constexpr auto ConcatArrays(const std::array<T, N>& lhs, const std::array<T, M>& rhs) {
  std::array<T, N + M> out{};
  for (std::size_t i = 0; i < N; ++i) {
    out[i] = lhs[i];
  }
  for (std::size_t i = 0; i < M; ++i) {
    out[N + i] = rhs[i];
  }
  return out;
}

constexpr bool ValidateTimingRule(const TimingRuleExpr& rule) {
  if (rule.node_count == 0) {
    return true;
  }
  if (rule.node_count > kMaxTimingRuleNodes || rule.root_index >= rule.node_count) {
    return false;
  }

  for (uint8_t i = 0; i < rule.node_count; ++i) {
    const TimingRuleNode& node = rule.nodes[i];
    switch (node.op) {
      case TimingRuleOp::kAlways:
      case TimingRuleOp::kCondition:
        break;
      case TimingRuleOp::kNot:
        if (node.lhs >= i) {
          return false;
        }
        break;
      case TimingRuleOp::kAllOf:
      case TimingRuleOp::kAnyOf:
        if (node.lhs >= i || node.rhs >= i) {
          return false;
        }
        break;
    }
  }
  return true;
}

constexpr bool ValidateOpcodeSpec(const OpcodeSpec& spec) {
  if (spec.disposition != OpcodeSpecDisposition::kImplemented) {
    return spec.cycle_count == 0 && !spec.overflowed;
  }
  if (spec.cycle_count == 0 || spec.cycle_count > kMaxRemainingOps || spec.overflowed) {
    return false;
  }
  for (uint8_t i = 0; i < spec.cycle_count; ++i) {
    if (!ValidateTimingRule(spec.cycles[i].rule)) {
      return false;
    }
  }
  return true;
}

template <std::size_t N>
constexpr bool ValidateUniqueOpcodes(const std::array<OpcodeSpec, N>& specs) {
  std::array<bool, 256> seen{};
  for (const OpcodeSpec& spec : specs) {
    if (seen[spec.opcode]) {
      return false;
    }
    seen[spec.opcode] = true;
  }
  return true;
}

constexpr bool EvaluateRuleForBits(const TimingRuleExpr& rule, uint32_t bits) {
  if (rule.node_count == 0) {
    return true;
  }
  std::array<bool, kMaxTimingRuleNodes> values{};
  for (uint8_t i = 0; i < rule.node_count; ++i) {
    const TimingRuleNode& node = rule.nodes[i];
    switch (node.op) {
      case TimingRuleOp::kAlways:
        values[i] = true;
        break;
      case TimingRuleOp::kCondition:
        values[i] = ((bits >> static_cast<uint8_t>(node.condition)) & 1U) != 0U;
        break;
      case TimingRuleOp::kNot:
        values[i] = !values[node.lhs];
        break;
      case TimingRuleOp::kAllOf:
        values[i] = values[node.lhs] && values[node.rhs];
        break;
      case TimingRuleOp::kAnyOf:
        values[i] = values[node.lhs] || values[node.rhs];
        break;
    }
  }
  return values[rule.root_index];
}

constexpr uint32_t ComputeTimingRuleTruthTable(const TimingRuleExpr& rule) {
  uint32_t table = 0;
  constexpr uint32_t kCombinations = 1U << kTimingConditionCount;
  for (uint32_t bits = 0; bits < kCombinations; ++bits) {
    if (EvaluateRuleForBits(rule, bits)) {
      table |= (1U << bits);
    }
  }
  return table;
}

constexpr uint8_t CountUniqueRules(const OpcodeSpec& spec) {
  std::array<uint32_t, kMaxInstructionRules> unique_tables{};
  uint8_t count = 1;
  unique_tables[0] = ComputeTimingRuleTruthTable(Always());

  for (uint8_t i = 0; i < spec.cycle_count; ++i) {
    const uint32_t table = ComputeTimingRuleTruthTable(spec.cycles[i].rule);
    bool found = false;
    for (uint8_t j = 0; j < count; ++j) {
      if (unique_tables[j] == table) {
        found = true;
        break;
      }
    }
    if (found) {
      continue;
    }
    if (count >= kMaxInstructionRules) {
      return static_cast<uint8_t>(kMaxInstructionRules + 1U);
    }
    unique_tables[count++] = table;
  }

  return count;
}

template <std::size_t N>
constexpr bool ValidateOpcodeSpecs(const std::array<OpcodeSpec, N>& specs) {
  if (!ValidateUniqueOpcodes(specs)) {
    return false;
  }
  for (const OpcodeSpec& spec : specs) {
    if (!ValidateOpcodeSpec(spec)) {
      return false;
    }
    if (CountUniqueRules(spec) > kMaxInstructionRules) {
      return false;
    }
  }
  return true;
}

constexpr uint8_t FindRuleIndex(const InstructionEntry& entry, uint32_t truth_table) {
  for (uint8_t i = 0; i < entry.rule_count; ++i) {
    if (entry.rules[i] == truth_table) {
      return i;
    }
  }
  return entry.rule_count;
}

constexpr InstructionEntry LowerOpcode(const OpcodeSpec& spec) {
  InstructionEntry entry{};
  if (spec.disposition != OpcodeSpecDisposition::kImplemented) {
    return entry;
  }

  entry.disposition = InstructionDisposition::kImplemented;
  entry.remaining_op_count = spec.cycle_count;
  entry.rule_count = 1;
  entry.rules[0] = ComputeTimingRuleTruthTable(Always());

  for (uint8_t i = 0; i < spec.cycle_count; ++i) {
    const CycleSlotSpec& slot = spec.cycles[i];
    const uint32_t table = ComputeTimingRuleTruthTable(slot.rule);
    uint8_t rule_index = FindRuleIndex(entry, table);
    if (rule_index == entry.rule_count) {
      entry.rules[entry.rule_count++] = table;
    }
    entry.ops[i] = MicroOp{slot.bus_action, slot.internal_op, slot.params, rule_index};
  }

  return entry;
}

constexpr OpcodeMetadata LowerMetadata(const OpcodeSpec& spec) {
  OpcodeMetadata metadata{};
  metadata.implemented = (spec.disposition == OpcodeSpecDisposition::kImplemented);
  metadata.mnemonic = spec.mnemonic;
  metadata.addressing_mode = spec.addressing_mode;
  metadata.cycle_count = spec.cycle_count;
  for (uint8_t i = 0; i < spec.cycle_count; ++i) {
    metadata.cycle_labels[i] = spec.cycles[i].label;
  }
  return metadata;
}

template <std::size_t N>
consteval OpcodeArtifacts BuildOpcodeArtifacts(const std::array<OpcodeSpec, N>& specs) noexcept {
  OpcodeArtifacts artifacts{};

  for (OpcodeMetadata& metadata : artifacts.metadata_table) {
    metadata = OpcodeMetadata{};
  }
  for (InstructionEntry& entry : artifacts.execution_table) {
    entry = InstructionEntry{};
  }

  for (const OpcodeSpec& spec : specs) {
    artifacts.execution_table[spec.opcode] = LowerOpcode(spec);
    artifacts.metadata_table[spec.opcode] = LowerMetadata(spec);
  }

  return artifacts;
}

extern const OpcodeArtifacts kOpcodeArtifacts;

}  // namespace pupsnes::opcode_defs_internal
