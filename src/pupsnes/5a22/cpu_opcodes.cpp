#include "cpu_opcode_defs_internal.h"

namespace pupsnes::opcode_defs_internal {
namespace {

constexpr CycleFragment FetchLongAddr() {
  return Fragment()
      .Then(FetchPc(MicroInternalOp::kSetAddrLowFromFetch, Always(), "fetch address low"))
      .Then(FetchPc(MicroInternalOp::kSetAddrHighFromFetch, Always(), "fetch address high"))
      .Then(FetchPc(MicroInternalOp::kSetAddrBankFromFetch, Always(), "fetch address bank"))
      .Build();
}

constexpr CycleFragment BranchSequence(MicroInternalOp branch_test_op) {
  return Fragment()
      .Then(FetchPc(branch_test_op, Always(), "fetch displacement"))
      .Then(Internal(MicroInternalOp::kBranchRelative8, Condition(TimingCondition::kBranchTaken), "apply branch"))
      .Build();
}

constexpr auto MakeMiscSpecs() {
  return std::array{
      Opcode(0xEA, "NOP", "implied").Then(Internal(MicroInternalOp::kNone, Always(), "idle")).Build(),
  };
}

constexpr auto MakeLoadSpecs() {
  return std::array{
      Opcode(0xA9, "LDA", "immediate")
          .Then(FetchPc(MicroInternalOp::kLoadALowUpdateNz, Always(), "fetch immediate"))
          .Build(),
      Opcode(0xA2, "LDX", "immediate")
          .Then(FetchPc(MicroInternalOp::kLoadXLowUpdateNz, Always(), "fetch immediate"))
          .Build(),
  };
}

constexpr auto MakeStoreSpecs() {
  return std::array{
      Opcode(0x8F, "STA", "absolute long")
          .Then(FetchLongAddr())
          .Then(WriteA8Addr(MicroInternalOp::kNone, Always(), "write A low"))
          .Build(),
  };
}

constexpr auto MakeBranchSpecs() {
  return std::array{
      Opcode(0x80, "BRA", "relative").Then(BranchSequence(MicroInternalOp::kSetBranchTaken)).Build(),
      Opcode(0xD0, "BNE", "relative").Then(BranchSequence(MicroInternalOp::kSetBranchTakenIfNotZero)).Build(),
  };
}

constexpr auto kExplicitOpcodeSpecs =
    ConcatArrays(ConcatArrays(MakeMiscSpecs(), MakeLoadSpecs()), ConcatArrays(MakeStoreSpecs(), MakeBranchSpecs()));

static_assert(ValidateOpcodeSpecs(kExplicitOpcodeSpecs), "Opcode specification validation failed");

}  // namespace

const OpcodeArtifacts kOpcodeArtifacts = BuildOpcodeArtifacts(kExplicitOpcodeSpecs);

}  // namespace pupsnes::opcode_defs_internal

namespace pupsnes {

const std::array<InstructionEntry, 256> CPU::kOpcodeTable = opcode_defs_internal::kOpcodeArtifacts.execution_table;

}  // namespace pupsnes
