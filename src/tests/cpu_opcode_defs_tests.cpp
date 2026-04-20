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
               {{B::kFetchPc, M::kLoadReg, 1, "fetch immediate low"},
                {B::kFetchPc, M::kLoadReg, 2, "fetch immediate low"},
                {B::kFetchPc, M::kLoadReg, 2, "fetch immediate high"}});

  ExpectOpcode(0xA2, "LDX", "immediate index", 3,
               {{B::kFetchPc, M::kLoadReg, 1, "fetch immediate low"},
                {B::kFetchPc, M::kLoadReg, 2, "fetch immediate low"},
                {B::kFetchPc, M::kLoadReg, 2, "fetch immediate high"}});

  ExpectOpcode(0xA0, "LDY", "immediate index", 3,
               {{B::kFetchPc, M::kLoadReg, 1, "fetch immediate low"},
                {B::kFetchPc, M::kLoadReg, 2, "fetch immediate low"},
                {B::kFetchPc, M::kLoadReg, 2, "fetch immediate high"}});

  ExpectOpcode(0x8D, "STA", "absolute", 2,
               {{B::kFetchPc, M::kSetAddrLowFromFetch, 0, "fetch address low"},
                {B::kFetchPc, M::kSetAddrHighFromFetchAndBankFromDbr, 0, "fetch address high"},
                {B::kWriteA8Addr, M::kIncrementAddr, 0, "write A low"},
                {B::kWriteAHighAddr, M::kNone, 1, "write A high"}});

  ExpectOpcode(0x8E, "STX", "absolute", 2,
               {{B::kFetchPc, M::kSetAddrLowFromFetch, 0, "fetch address low"},
                {B::kFetchPc, M::kSetAddrHighFromFetchAndBankFromDbr, 0, "fetch address high"},
                {B::kWriteX8Addr, M::kIncrementAddr, 0, "write X low"},
                {B::kWriteXHighAddr, M::kNone, 1, "write X high"}});

  ExpectOpcode(0x8C, "STY", "absolute", 2,
               {{B::kFetchPc, M::kSetAddrLowFromFetch, 0, "fetch address low"},
                {B::kFetchPc, M::kSetAddrHighFromFetchAndBankFromDbr, 0, "fetch address high"},
                {B::kWriteY8Addr, M::kIncrementAddr, 0, "write Y low"},
                {B::kWriteYHighAddr, M::kNone, 1, "write Y high"}});

  ExpectOpcode(0x8F, "STA", "absolute long", 2,
               {{B::kFetchPc, M::kSetAddrLowFromFetch, 0, "fetch address low"},
                {B::kFetchPc, M::kSetAddrHighFromFetch, 0, "fetch address high"},
                {B::kFetchPc, M::kSetAddrBankFromFetch, 0, "fetch address bank"},
                {B::kWriteA8Addr, M::kIncrementAddr, 0, "write A low"},
                {B::kWriteAHighAddr, M::kNone, 1, "write A high"}});

  ExpectOpcode(0x80, "BRA", "relative", 3,
               {{B::kFetchPc, M::kSetBranchTakenCond, 0, "fetch displacement"},
                {B::kNone, M::kBranchRelative8, 1, "apply branch"},
                {B::kNone, M::kNone, 2, "emulation page-cross penalty"}});

  ExpectOpcode(0xD0, "BNE", "relative", 3,
               {{B::kFetchPc, M::kSetBranchTakenCond, 0, "fetch displacement"},
                {B::kNone, M::kBranchRelative8, 1, "apply branch"},
                {B::kNone, M::kNone, 2, "emulation page-cross penalty"}});

  const uint32_t bne_branch_rule = kOpcodeArtifacts.execution_table[0xD0].rules[1];
  REQUIRE(bne_branch_rule == ComputeTimingRuleTruthTable(Condition(TimingCondition::kBranchTaken)));

  const uint32_t bne_penalty_rule = kOpcodeArtifacts.execution_table[0xD0].rules[2];
  REQUIRE(bne_penalty_rule ==
          ComputeTimingRuleTruthTable(
              AllOf(Condition(TimingCondition::kEmulationMode), Condition(TimingCondition::kBranchPageCrossed))));

  ExpectOpcode(0x48, "PHA", "implied", 2,
               {{B::kNone, M::kNone, 0, "internal"},
                {B::kPushStack, M::kDecrementSp, 1, "push A high"},
                {B::kPushStack, M::kDecrementSp, 0, "push A low"}});

  const uint32_t pha_high_rule = kOpcodeArtifacts.execution_table[0x48].rules[1];
  REQUIRE(pha_high_rule == ComputeTimingRuleTruthTable(Condition(TimingCondition::kAccumulator16)));

  ExpectOpcode(0x8B, "PHB", "implied", 1,
               {{B::kNone, M::kNone, 0, "internal"}, {B::kPushStack, M::kDecrementSp, 0, "push DBR"}});

  ExpectOpcode(0xAB, "PLB", "implied", 1,
               {{B::kNone, M::kNone, 0, "internal"},
                {B::kNone, M::kIncrementSp, 0, "increment SP"},
                {B::kPullStack, M::kLoadDbrUpdateNz, 0, "pull DBR"}});
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
