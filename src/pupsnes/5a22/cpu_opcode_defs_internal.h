#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "pupsnes/hw/5a22/cpu.h"

namespace pupsnes::opcode_defs_internal {

struct CycleSlotSpec {
  MicroBusAction bus_action = MicroBusAction::kNone;
  MicroInternalOp internal_op = MicroInternalOp::kNone;
  TimingRuleExpr rule{};
  std::string_view label{};
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

constexpr TimingRuleNode ShiftTimingRuleNode(const TimingRuleNode& node,
                                             uint8_t offset) {
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

  const uint8_t copy_count = (expr.node_count < kMaxTimingRuleNodes)
                                 ? expr.node_count
                                 : kMaxTimingRuleNodes;
  for (uint8_t i = 0; i < copy_count; ++i) {
    out.nodes[i] = expr.nodes[i];
  }
  if (out.root_index < kMaxTimingRuleNodes) {
    out.nodes[out.root_index].op = TimingRuleOp::kNot;
    out.nodes[out.root_index].lhs = expr.root_index;
  }
  return out;
}

constexpr TimingRuleExpr MergeRules(TimingRuleOp op, const TimingRuleExpr& lhs,
                                    const TimingRuleExpr& rhs) {
  TimingRuleExpr out{};
  const uint8_t rhs_offset = lhs.node_count;
  out.node_count = static_cast<uint8_t>(lhs.node_count + rhs.node_count + 1U);
  out.root_index = static_cast<uint8_t>(lhs.node_count + rhs.node_count);

  for (uint8_t i = 0; i < lhs.node_count && i < kMaxTimingRuleNodes; ++i) {
    out.nodes[i] = lhs.nodes[i];
  }
  for (uint8_t i = 0;
       i < rhs.node_count &&
       static_cast<uint8_t>(rhs_offset + i) < kMaxTimingRuleNodes;
       ++i) {
    out.nodes[rhs_offset + i] = ShiftTimingRuleNode(rhs.nodes[i], rhs_offset);
  }
  if (out.root_index < kMaxTimingRuleNodes) {
    out.nodes[out.root_index].op = op;
    out.nodes[out.root_index].lhs = lhs.root_index;
    out.nodes[out.root_index].rhs =
        static_cast<uint8_t>(rhs.root_index + rhs_offset);
  }
  return out;
}

constexpr TimingRuleExpr AllOf(const TimingRuleExpr& lhs,
                               const TimingRuleExpr& rhs) {
  return MergeRules(TimingRuleOp::kAllOf, lhs, rhs);
}

constexpr TimingRuleExpr AnyOf(const TimingRuleExpr& lhs,
                               const TimingRuleExpr& rhs) {
  return MergeRules(TimingRuleOp::kAnyOf, lhs, rhs);
}

constexpr CycleSlotSpec FetchPc(
    MicroInternalOp internal_op = MicroInternalOp::kNone,
    TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{MicroBusAction::kFetchPc, internal_op, rule, label};
}

constexpr CycleSlotSpec ReadAddr(
    MicroInternalOp internal_op = MicroInternalOp::kNone,
    TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{MicroBusAction::kReadAddr, internal_op, rule, label};
}

constexpr CycleSlotSpec WriteA8Addr(
    MicroInternalOp internal_op = MicroInternalOp::kNone,
    TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{MicroBusAction::kWriteA8Addr, internal_op, rule, label};
}

constexpr CycleSlotSpec Internal(
    MicroInternalOp internal_op = MicroInternalOp::kNone,
    TimingRuleExpr rule = Always(), std::string_view label = {}) {
  return CycleSlotSpec{MicroBusAction::kNone, internal_op, rule, label};
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

  constexpr OpcodeSpecBuilder(uint8_t opcode, std::string_view mnemonic,
                              std::string_view addressing_mode) {
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

constexpr OpcodeSpecBuilder Opcode(uint8_t opcode, std::string_view mnemonic,
                                   std::string_view addressing_mode) {
  return OpcodeSpecBuilder(opcode, mnemonic, addressing_mode);
}

template <typename T, std::size_t N, std::size_t M>
constexpr auto ConcatArrays(const std::array<T, N>& lhs,
                            const std::array<T, M>& rhs) {
  std::array<T, N + M> out{};
  for (std::size_t i = 0; i < N; ++i) {
    out[i] = lhs[i];
  }
  for (std::size_t i = 0; i < M; ++i) {
    out[N + i] = rhs[i];
  }
  return out;
}

constexpr bool TimingRuleNodesEqual(const TimingRuleNode& lhs,
                                    const TimingRuleNode& rhs) {
  return lhs.op == rhs.op && lhs.condition == rhs.condition &&
         lhs.lhs == rhs.lhs && lhs.rhs == rhs.rhs;
}

constexpr bool TimingRulesEqual(const TimingRuleExpr& lhs,
                                const TimingRuleExpr& rhs) {
  if (lhs.node_count != rhs.node_count || lhs.root_index != rhs.root_index) {
    return false;
  }
  for (uint8_t i = 0; i < lhs.node_count; ++i) {
    if (!TimingRuleNodesEqual(lhs.nodes[i], rhs.nodes[i])) {
      return false;
    }
  }
  return true;
}

constexpr bool ValidateTimingRule(const TimingRuleExpr& rule) {
  if (rule.node_count == 0) {
    return true;
  }
  if (rule.node_count > kMaxTimingRuleNodes ||
      rule.root_index >= rule.node_count) {
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
  if (spec.cycle_count == 0 || spec.cycle_count > kMaxRemainingOps ||
      spec.overflowed) {
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

constexpr uint8_t CountUniqueRules(const OpcodeSpec& spec) {
  std::array<TimingRuleExpr, kMaxInstructionRules> unique_rules{};
  uint8_t count = 1;
  unique_rules[0] = Always();

  for (uint8_t i = 0; i < spec.cycle_count; ++i) {
    bool found = false;
    for (uint8_t j = 0; j < count; ++j) {
      if (TimingRulesEqual(unique_rules[j], spec.cycles[i].rule)) {
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
    unique_rules[count++] = spec.cycles[i].rule;
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

constexpr uint8_t FindRuleIndex(const InstructionEntry& entry,
                                const TimingRuleExpr& rule) {
  for (uint8_t i = 0; i < entry.rule_count; ++i) {
    if (TimingRulesEqual(entry.rules[i], rule)) {
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
  entry.rules[0] = Always();

  for (uint8_t i = 0; i < spec.cycle_count; ++i) {
    const CycleSlotSpec& slot = spec.cycles[i];
    uint8_t rule_index = FindRuleIndex(entry, slot.rule);
    if (rule_index == entry.rule_count) {
      entry.rules[entry.rule_count++] = slot.rule;
    }
    entry.ops[i] = MicroOp{slot.bus_action, slot.internal_op, rule_index};
  }

  return entry;
}

constexpr OpcodeMetadata LowerMetadata(const OpcodeSpec& spec) {
  OpcodeMetadata metadata{};
  metadata.implemented =
      (spec.disposition == OpcodeSpecDisposition::kImplemented);
  metadata.mnemonic = spec.mnemonic;
  metadata.addressing_mode = spec.addressing_mode;
  metadata.cycle_count = spec.cycle_count;
  for (uint8_t i = 0; i < spec.cycle_count; ++i) {
    metadata.cycle_labels[i] = spec.cycles[i].label;
  }
  return metadata;
}

template <std::size_t N>
consteval OpcodeArtifacts BuildOpcodeArtifacts(
    const std::array<OpcodeSpec, N>& specs) {
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
