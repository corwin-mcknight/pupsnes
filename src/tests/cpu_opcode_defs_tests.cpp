#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <initializer_list>
#include <string_view>

#include "cpu_spec_oracle.h"
#include "pupsnes/hw/5a22/addressing_fragments.h"
#include "pupsnes/hw/5a22/cpu_opcode_defs_internal.h"

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
                                    .Then(Internal(MicroInternalOp::kNone, Always(), "c8"))
                                    .Build();

static_assert(!ValidateUniqueOpcodes(kDuplicateOpcodeSpecs));
static_assert(!ValidateOpcodeSpec(kOversizedSpec));

// Execution properties follow composed addressing fragments, not their
// display label. This also covers indirect modes that add pointer reads.
constexpr auto kRelabeledIndirect = Opcode(0xB2, "LDA", "renamed indirect mode")
                                        .Then(FetchDirectPage())
                                        .Then(FetchDirectIndirect())
                                        .Then(LoadRegFromAddr(Reg::kA, TimingCondition::kAccumulator16))
                                        .Build();
static_assert(ValidateOpcodeSpec(kRelabeledIndirect));
static_assert(LowerOpcode(kRelabeledIndirect).uses_dp_penalty);

constexpr auto kConflictingTimingSources =
    Opcode(0xB2, "BAD", "test").Then(FetchDirectPage()).Then(FetchAbsoluteIndexedRead(Reg::kX)).Build();
static_assert(!ValidateOpcodeSpec(kConflictingTimingSources));

constexpr auto kDpWithUnconditionalBranch =
    Opcode(0x80, "BAD", "test").Then(FetchDirectPage()).Then(BranchRelative(false)).Build();
static_assert(!ValidateOpcodeSpec(kDpWithUnconditionalBranch));

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
               {{B::kFetchPc, M::kSetAddrByteFromFetch, 0, "fetch address low"},
                {B::kFetchPc, M::kSetAddrByteFromFetch, 0, "fetch address high"},
                {B::kWriteRegByte, M::kModifyAddr, 0, "write A low"},
                {B::kWriteRegByte, M::kNone, 1, "write A high"}});

  ExpectOpcode(0x8E, "STX", "absolute", 2,
               {{B::kFetchPc, M::kSetAddrByteFromFetch, 0, "fetch address low"},
                {B::kFetchPc, M::kSetAddrByteFromFetch, 0, "fetch address high"},
                {B::kWriteRegByte, M::kModifyAddr, 0, "write X low"},
                {B::kWriteRegByte, M::kNone, 1, "write X high"}});

  ExpectOpcode(0x8C, "STY", "absolute", 2,
               {{B::kFetchPc, M::kSetAddrByteFromFetch, 0, "fetch address low"},
                {B::kFetchPc, M::kSetAddrByteFromFetch, 0, "fetch address high"},
                {B::kWriteRegByte, M::kModifyAddr, 0, "write Y low"},
                {B::kWriteRegByte, M::kNone, 1, "write Y high"}});

  ExpectOpcode(0x8F, "STA", "absolute long", 2,
               {{B::kFetchPc, M::kSetAddrByteFromFetch, 0, "fetch address low"},
                {B::kFetchPc, M::kSetAddrByteFromFetch, 0, "fetch address high"},
                {B::kFetchPc, M::kSetAddrByteFromFetch, 0, "fetch address bank"},
                {B::kWriteRegByte, M::kModifyAddr, 0, "write A low"},
                {B::kWriteRegByte, M::kNone, 1, "write A high"}});

  ExpectOpcode(0x80, "BRA", "relative", 3,
               {{B::kFetchPc, M::kSetBranchTakenCond, 0, "fetch displacement"},
                {B::kNone, M::kBranchRelative, 1, "apply branch"},
                {B::kNone, M::kNone, 2, "emulation page-cross penalty"}});

  ExpectOpcode(0xD0, "BNE", "relative", 3,
               {{B::kFetchPc, M::kSetBranchTakenCond, 0, "fetch displacement"},
                {B::kNone, M::kBranchRelative, 1, "apply branch"},
                {B::kNone, M::kNone, 2, "emulation page-cross penalty"}});

  const uint32_t bne_branch_rule = kOpcodeArtifacts.execution_table[0xD0].rules[1];
  REQUIRE(bne_branch_rule == ComputeTimingRuleTruthTable(Condition(TimingCondition::kBranchTaken)));

  const uint32_t bne_penalty_rule = kOpcodeArtifacts.execution_table[0xD0].rules[2];
  REQUIRE(bne_penalty_rule == ComputeTimingRuleTruthTable(AllOf(Condition(TimingCondition::kEmulationMode),
                                                                Condition(TimingCondition::kBranchPageCrossed))));

  ExpectOpcode(0x48, "PHA", "implied", 2,
               {{B::kNone, M::kNone, 0, "internal"},
                {B::kPushStack, M::kModifySp, 1, "push A high"},
                {B::kPushStack, M::kModifySp, 0, "push A low"}});

  const uint32_t pha_high_rule = kOpcodeArtifacts.execution_table[0x48].rules[1];
  REQUIRE(pha_high_rule == ComputeTimingRuleTruthTable(Condition(TimingCondition::kAccumulator16)));

  ExpectOpcode(0x8B, "PHB", "implied", 1,
               {{B::kNone, M::kNone, 0, "internal"}, {B::kPushStack, M::kModifySp, 0, "push DBR"}});

  ExpectOpcode(0xAB, "PLB", "implied", 1,
               {{B::kNone, M::kNone, 0, "internal"},
                {B::kNone, M::kModifySp, 0, "increment SP"},
                {B::kPullStack, M::kLoadReg, 0, "pull DBR"}});
}

TEST_CASE("Every implemented opcode matches the 65C816 spec", "[cpu][opcode-defs]") {
  using cpu_spec_oracle::CountActiveCycles;
  using cpu_spec_oracle::EvalFormula;
  using cpu_spec_oracle::FindSpec;
  using cpu_spec_oracle::FormulaInputs;
  using cpu_spec_oracle::NormalizeAddressing;
  using cpu_spec_oracle::PackConditionBits;
  using cpu_spec_oracle::SpecEntry;

  for (unsigned op = 0; op < 256; ++op) {
    const auto& entry = kOpcodeArtifacts.execution_table[op];
    const auto& meta = kOpcodeArtifacts.metadata_table[op];
    if (entry.disposition != InstructionDisposition::kImplemented) {
      continue;
    }

    CAPTURE(op);
    const SpecEntry* spec = FindSpec(static_cast<uint8_t>(op));
    INFO("opcode " << op << " implemented but missing from kSpec — add a row");
    REQUIRE(spec != nullptr);

    // Mnemonic is the 3-letter canonical form in both tables.
    REQUIRE(meta.mnemonic == spec->mnemonic);

    // Addressing: the lowered metadata stores the project's internal label
    // (e.g. "immediate index"); the spec stores Clark's canonical token
    // ("imm"). Route the metadata label through NormalizeAddressing and the
    // two must agree.
    const std::string_view normalized = NormalizeAddressing(meta.addressing_mode);
    INFO("addressing normalize: '" << meta.addressing_mode << "' -> '" << normalized << "' vs spec '"
                                   << spec->addressing << "'");
    REQUIRE(normalized == spec->addressing);
    REQUIRE(entry.uses_dp_penalty == spec->dp_penalty_bit);

    // Cycle-formula conformance: evaluate Clark's formula at six
    // representative mode configurations and verify the lowered table
    // dispatches the same number of cycles (including the implicit opcode
    // fetch). Include both zero and nonzero DP low bytes in each width.
    constexpr std::array<FormulaInputs, 6> kModes = {{
        // m=1, x=1, native, no branch taken / not page-crossed
        {.m = 1, .x = 1, .w = 0, .p = 0, .t = 0, .e = 0},
        // m=0, x=0, native — exercise 16-bit cycle penalties
        {.m = 0, .x = 0, .w = 0, .p = 0, .t = 0, .e = 0},
        // branch taken, native, no page cross
        {.m = 1, .x = 1, .w = 0, .p = 0, .t = 1, .e = 0},
        // branch taken, emulation, page crossed — exercises t*e*p
        {.m = 1, .x = 1, .w = 0, .p = 1, .t = 1, .e = 1},
        {.m = 1, .x = 1, .w = 1, .p = 0, .t = 0, .e = 0},
        {.m = 0, .x = 0, .w = 1, .p = 0, .t = 0, .e = 0},
    }};

    for (const FormulaInputs& orig : kModes) {
      // DP opcodes share bit 4 with branch/index page crossing. Only pack
      // the source used by this opcode so an unrelated input cannot fire it.
      FormulaInputs in = orig;
      if (spec->dp_penalty_bit) {
        in.p = 0;
      } else {
        in.w = 0;
      }
      CAPTURE(in.m, in.x, in.t, in.p, in.e, in.w);
      const int expected = EvalFormula(spec->cycle_formula, in);
      const int actual = CountActiveCycles(entry, PackConditionBits(in) | spec->forced_condition_bits);
      INFO("cycle_formula '" << spec->cycle_formula << "' expected=" << expected << " actual=" << actual);
      REQUIRE(expected == actual);
    }
  }
}

TEST_CASE("Opcodes absent from the spec oracle are marked unimplemented", "[cpu][opcode-defs]") {
  using cpu_spec_oracle::FindSpec;

  for (unsigned op = 0; op < 256; ++op) {
    if (FindSpec(static_cast<uint8_t>(op)) != nullptr) {
      continue;
    }
    CAPTURE(op);
    const auto& entry = kOpcodeArtifacts.execution_table[op];
    INFO("opcode " << op << " not in kSpec but implementation isn't a fault");
    REQUIRE(entry.disposition == InstructionDisposition::kFaultUnimplemented);
  }
}
