#include <array>
#include <catch2/catch_test_macros.hpp>

#include "pupsnes/5a22/cpu_opcode_defs_internal.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)
using namespace pupsnes::
    opcode_defs_internal;  // NOLINT(google-build-using-namespace)

namespace {

constexpr auto kDuplicateOpcodeSpecs = std::array{
    Opcode(0xEA, "NOP", "implied")
        .Then(Internal(MicroInternalOp::kNone, Always(), "idle"))
        .Build(),
    Opcode(0xEA, "ALT", "test")
        .Then(Internal(MicroInternalOp::kNone, Always(), "duplicate"))
        .Build(),
};

constexpr auto kOversizedSpec =
    Opcode(0x01, "OVR", "test")
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

}  // namespace

TEST_CASE("Opcode specs lower into expected execution entries",
          "[cpu][opcode-defs]") {
  const auto& table = kOpcodeArtifacts.execution_table;

  const InstructionEntry& nop = table[0xEA];
  REQUIRE(nop.disposition == InstructionDisposition::kImplemented);
  REQUIRE(nop.remaining_op_count == 1);
  REQUIRE(nop.rule_count == 1);
  REQUIRE(nop.ops[0].bus_action == MicroBusAction::kNone);
  REQUIRE(nop.ops[0].internal_op == MicroInternalOp::kNone);
  REQUIRE(nop.ops[0].rule_index == 0);

  const InstructionEntry& lda_imm = table[0xA9];
  REQUIRE(lda_imm.disposition == InstructionDisposition::kImplemented);
  REQUIRE(lda_imm.remaining_op_count == 1);
  REQUIRE(lda_imm.ops[0].bus_action == MicroBusAction::kFetchPc);
  REQUIRE(lda_imm.ops[0].internal_op == MicroInternalOp::kLoadALowUpdateNz);

  const InstructionEntry& ldx_imm = table[0xA2];
  REQUIRE(ldx_imm.disposition == InstructionDisposition::kImplemented);
  REQUIRE(ldx_imm.remaining_op_count == 1);
  REQUIRE(ldx_imm.ops[0].bus_action == MicroBusAction::kFetchPc);
  REQUIRE(ldx_imm.ops[0].internal_op == MicroInternalOp::kLoadXLowUpdateNz);

  const InstructionEntry& sta_long = table[0x8F];
  REQUIRE(sta_long.disposition == InstructionDisposition::kImplemented);
  REQUIRE(sta_long.remaining_op_count == 4);
  REQUIRE(sta_long.ops[0].internal_op == MicroInternalOp::kSetAddrLowFromFetch);
  REQUIRE(sta_long.ops[1].internal_op ==
          MicroInternalOp::kSetAddrHighFromFetch);
  REQUIRE(sta_long.ops[2].internal_op ==
          MicroInternalOp::kSetAddrBankFromFetch);
  REQUIRE(sta_long.ops[3].bus_action == MicroBusAction::kWriteA8Addr);

  const InstructionEntry& bra = table[0x80];
  REQUIRE(bra.disposition == InstructionDisposition::kImplemented);
  REQUIRE(bra.remaining_op_count == 2);
  REQUIRE(bra.rule_count == 2);
  REQUIRE(bra.ops[0].internal_op == MicroInternalOp::kSetBranchTaken);
  REQUIRE(bra.ops[1].internal_op == MicroInternalOp::kBranchRelative8);
  REQUIRE(bra.ops[1].rule_index == 1);

  const InstructionEntry& bne = table[0xD0];
  REQUIRE(bne.disposition == InstructionDisposition::kImplemented);
  REQUIRE(bne.remaining_op_count == 2);
  REQUIRE(bne.rule_count == 2);
  REQUIRE(bne.ops[0].bus_action == MicroBusAction::kFetchPc);
  REQUIRE(bne.ops[0].internal_op == MicroInternalOp::kSetBranchTakenIfNotZero);
  REQUIRE(bne.ops[1].bus_action == MicroBusAction::kNone);
  REQUIRE(bne.ops[1].internal_op == MicroInternalOp::kBranchRelative8);
  REQUIRE(bne.ops[1].rule_index == 1);
  REQUIRE(bne.rules[1].node_count == 1);
  REQUIRE(bne.rules[1].nodes[0].op == TimingRuleOp::kCondition);
  REQUIRE(bne.rules[1].nodes[0].condition == TimingCondition::kBranchTaken);
}

TEST_CASE("Opcode metadata preserves readable lowered cycle labels",
          "[cpu][opcode-defs]") {
  const auto& metadata = kOpcodeArtifacts.metadata_table;

  REQUIRE(metadata[0xEA].implemented);
  REQUIRE(metadata[0xEA].mnemonic == "NOP");
  REQUIRE(metadata[0xEA].cycle_labels[0] == "idle");

  REQUIRE(metadata[0x8F].implemented);
  REQUIRE(metadata[0x8F].mnemonic == "STA");
  REQUIRE(metadata[0x8F].addressing_mode == "absolute long");
  REQUIRE(metadata[0x8F].cycle_labels[0] == "fetch address low");
  REQUIRE(metadata[0x8F].cycle_labels[1] == "fetch address high");
  REQUIRE(metadata[0x8F].cycle_labels[2] == "fetch address bank");
  REQUIRE(metadata[0x8F].cycle_labels[3] == "write A low");
}

TEST_CASE("Unimplemented opcodes lower to explicit fault entries",
          "[cpu][opcode-defs]") {
  const InstructionEntry& entry = kOpcodeArtifacts.execution_table[0x00];
  const OpcodeMetadata& metadata = kOpcodeArtifacts.metadata_table[0x00];

  REQUIRE(entry.disposition == InstructionDisposition::kFaultUnimplemented);
  REQUIRE(entry.remaining_op_count == 0);
  REQUIRE(entry.rule_count == 0);

  REQUIRE_FALSE(metadata.implemented);
  REQUIRE(metadata.mnemonic == "???");
  REQUIRE(metadata.cycle_count == 0);
}
