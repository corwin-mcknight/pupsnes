#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <initializer_list>

#include "pupsnes/5a22/cpu_opcode_defs_internal.h"

using namespace pupsnes;                        // NOLINT(google-build-using-namespace)
using namespace pupsnes::opcode_defs_internal;  // NOLINT(google-build-using-namespace)

namespace {

constexpr auto kDuplicateOpcodeSpecs = std::array{
    Opcode(0xEA, "NOP", "implied").Then(Internal(MicroInternalOp::kNone, Always(), "idle")).Build(),
    Opcode(0xEA, "ALT", "test").Then(Internal(MicroInternalOp::kNone, Always(), "duplicate")).Build(),
};

constexpr auto kOversizedSpec = Opcode(0x01, "OVR", "test")
                                    .Then(Internal(MicroInternalOp::kNone, Always(), "c0"))
                                    .Then(Internal(MicroInternalOp::kNone, Always(), "c1"))
                                    .Then(Internal(MicroInternalOp::kNone, Always(), "c2"))
                                    .Then(Internal(MicroInternalOp::kNone, Always(), "c3"))
                                    .Then(Internal(MicroInternalOp::kNone, Always(), "c4"))
                                    .Then(Internal(MicroInternalOp::kNone, Always(), "c5"))
                                    .Then(Internal(MicroInternalOp::kNone, Always(), "c6"))
                                    .Then(Internal(MicroInternalOp::kNone, Always(), "c7"))
                                    .Build();

static_assert(!ValidateUniqueOpcodes(kDuplicateOpcodeSpecs));
static_assert(!ValidateOpcodeSpec(kOversizedSpec));

struct CycleExpect {
  MicroBusAction bus;
  MicroInternalOp op;
  uint8_t rule_index;
  const char* label;
};

void ExpectOpcode(uint8_t opcode, const char* mnemonic, const char* addressing, uint8_t rule_count,
                  std::initializer_list<CycleExpect> cycles) {
  CAPTURE(opcode);
  const auto& e = kOpcodeArtifacts.execution_table[opcode];
  REQUIRE(e.disposition == InstructionDisposition::kImplemented);
  REQUIRE(e.remaining_op_count == cycles.size());
  REQUIRE(e.rule_count == rule_count);

  const auto& m = kOpcodeArtifacts.metadata_table[opcode];
  REQUIRE(m.implemented);
  REQUIRE(m.mnemonic == mnemonic);
  REQUIRE(m.addressing_mode == addressing);
  REQUIRE(m.cycle_count == cycles.size());

  std::size_t i = 0;
  for (const auto& c : cycles) {
    CAPTURE(i);
    REQUIRE(e.ops[i].bus_action == c.bus);
    REQUIRE(e.ops[i].internal_op == c.op);
    REQUIRE(e.ops[i].rule_index == c.rule_index);
    REQUIRE(m.cycle_labels[i] == c.label);
    ++i;
  }
}

}  // namespace

TEST_CASE("Opcode specs lower into expected execution and metadata entries", "[cpu][opcode-defs]") {
  using B = MicroBusAction;
  using M = MicroInternalOp;

  ExpectOpcode(0xEA, "NOP", "implied", 1, {{B::kNone, M::kNone, 0, "idle"}});

  ExpectOpcode(0xA9, "LDA", "immediate", 3,
               {{B::kFetchPc, M::kLoadA8UpdateNz, 1, "fetch immediate low"},
                {B::kFetchPc, M::kLoadALow, 2, "fetch immediate low"},
                {B::kFetchPc, M::kLoadAHighUpdateNz, 2, "fetch immediate high"}});

  ExpectOpcode(0xA2, "LDX", "immediate", 3,
               {{B::kFetchPc, M::kLoadX8UpdateNz, 1, "fetch immediate low"},
                {B::kFetchPc, M::kLoadXLow, 2, "fetch immediate low"},
                {B::kFetchPc, M::kLoadXHighUpdateNz, 2, "fetch immediate high"}});

  ExpectOpcode(0xA0, "LDY", "immediate", 3,
               {{B::kFetchPc, M::kLoadY8UpdateNz, 1, "fetch immediate low"},
                {B::kFetchPc, M::kLoadYLow, 2, "fetch immediate low"},
                {B::kFetchPc, M::kLoadYHighUpdateNz, 2, "fetch immediate high"}});

  ExpectOpcode(0x8F, "STA", "absolute long", 1,
               {{B::kFetchPc, M::kSetAddrLowFromFetch, 0, "fetch address low"},
                {B::kFetchPc, M::kSetAddrHighFromFetch, 0, "fetch address high"},
                {B::kFetchPc, M::kSetAddrBankFromFetch, 0, "fetch address bank"},
                {B::kWriteA8Addr, M::kNone, 0, "write A low"}});

  ExpectOpcode(
      0x80, "BRA", "relative", 2,
      {{B::kFetchPc, M::kSetBranchTaken, 0, "fetch displacement"}, {B::kNone, M::kBranchRelative8, 1, "apply branch"}});

  ExpectOpcode(0xD0, "BNE", "relative", 2,
               {{B::kFetchPc, M::kSetBranchTakenIfNotZero, 0, "fetch displacement"},
                {B::kNone, M::kBranchRelative8, 1, "apply branch"}});

  const auto& bne_branch_rule = kOpcodeArtifacts.execution_table[0xD0].rules[1];
  REQUIRE(bne_branch_rule.node_count == 1);
  REQUIRE(bne_branch_rule.nodes[0].op == TimingRuleOp::kCondition);
  REQUIRE(bne_branch_rule.nodes[0].condition == TimingCondition::kBranchTaken);
}

TEST_CASE("Unimplemented opcodes lower to explicit fault entries", "[cpu][opcode-defs]") {
  const InstructionEntry& entry = kOpcodeArtifacts.execution_table[0x00];
  const OpcodeMetadata& metadata = kOpcodeArtifacts.metadata_table[0x00];

  REQUIRE(entry.disposition == InstructionDisposition::kFaultUnimplemented);
  REQUIRE(entry.remaining_op_count == 0);
  REQUIRE(entry.rule_count == 0);

  REQUIRE_FALSE(metadata.implemented);
  REQUIRE(metadata.mnemonic == "???");
  REQUIRE(metadata.cycle_count == 0);
}
