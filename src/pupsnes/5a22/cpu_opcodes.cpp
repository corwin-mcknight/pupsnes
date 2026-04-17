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

constexpr CycleFragment FetchAbsoluteAddr() {
  return Fragment()
      .Then(FetchPc(MicroInternalOp::kSetAddrLowFromFetch, Always(), "fetch address low"))
      .Then(FetchPc(MicroInternalOp::kSetAddrHighFromFetchAndBankFromDbr, Always(), "fetch address high"))
      .Build();
}

constexpr CycleFragment LoadAccumulatorImmediate() {
  return Fragment()
      .Then(FetchPc(MicroInternalOp::kLoadA8UpdateNz, Not(Condition(TimingCondition::kAccumulator16)),
                    "fetch immediate low"))
      .Then(FetchPc(MicroInternalOp::kLoadALow, Condition(TimingCondition::kAccumulator16), "fetch immediate low"))
      .Then(FetchPc(MicroInternalOp::kLoadAHighUpdateNz, Condition(TimingCondition::kAccumulator16),
                    "fetch immediate high"))
      .Build();
}

constexpr CycleFragment LoadIndexXImmediate() {
  return Fragment()
      .Then(FetchPc(MicroInternalOp::kLoadX8UpdateNz, Not(Condition(TimingCondition::kIndex16)), "fetch immediate low"))
      .Then(FetchPc(MicroInternalOp::kLoadXLow, Condition(TimingCondition::kIndex16), "fetch immediate low"))
      .Then(FetchPc(MicroInternalOp::kLoadXHighUpdateNz, Condition(TimingCondition::kIndex16), "fetch immediate high"))
      .Build();
}

constexpr CycleFragment LoadIndexYImmediate() {
  return Fragment()
      .Then(FetchPc(MicroInternalOp::kLoadY8UpdateNz, Not(Condition(TimingCondition::kIndex16)), "fetch immediate low"))
      .Then(FetchPc(MicroInternalOp::kLoadYLow, Condition(TimingCondition::kIndex16), "fetch immediate low"))
      .Then(FetchPc(MicroInternalOp::kLoadYHighUpdateNz, Condition(TimingCondition::kIndex16), "fetch immediate high"))
      .Build();
}

constexpr CycleFragment BranchSequence(MicroInternalOp branch_test_op) {
  return Fragment()
      .Then(FetchPc(branch_test_op, Always(), "fetch displacement"))
      .Then(Internal(MicroInternalOp::kBranchRelative8, Condition(TimingCondition::kBranchTaken), "apply branch"))
      .Then(Internal(MicroInternalOp::kNone,
                     AllOf(Condition(TimingCondition::kEmulationMode), Condition(TimingCondition::kBranchPageCrossed)),
                     "emulation page-cross penalty"))
      .Build();
}

constexpr CycleFragment StoreAccumulator() {
  return Fragment()
      .Then(WriteA8Addr(MicroInternalOp::kIncrementAddr, Always(), "write A low"))
      .Then(WriteAHighAddr(MicroInternalOp::kNone, Condition(TimingCondition::kAccumulator16), "write A high"))
      .Build();
}

constexpr CycleFragment StoreIndexX() {
  return Fragment()
      .Then(WriteX8Addr(MicroInternalOp::kIncrementAddr, Always(), "write X low"))
      .Then(WriteXHighAddr(MicroInternalOp::kNone, Condition(TimingCondition::kIndex16), "write X high"))
      .Build();
}

constexpr CycleFragment StoreIndexY() {
  return Fragment()
      .Then(WriteY8Addr(MicroInternalOp::kIncrementAddr, Always(), "write Y low"))
      .Then(WriteYHighAddr(MicroInternalOp::kNone, Condition(TimingCondition::kIndex16), "write Y high"))
      .Build();
}

constexpr CycleFragment PushAccumulator() {
  return Fragment()
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(PushAHigh(MicroInternalOp::kDecrementSp, Condition(TimingCondition::kAccumulator16), "push A high"))
      .Then(PushA8(MicroInternalOp::kDecrementSp, Always(), "push A low"))
      .Build();
}

constexpr CycleFragment PushDataBank() {
  return Fragment()
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(PushDbr(MicroInternalOp::kDecrementSp, Always(), "push DBR"))
      .Build();
}

constexpr CycleFragment PullDataBank() {
  return Fragment()
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(Internal(MicroInternalOp::kIncrementSp, Always(), "increment SP"))
      .Then(PullStack(MicroInternalOp::kLoadDbrUpdateNz, Always(), "pull DBR"))
      .Build();
}

constexpr auto MakeMiscSpecs() {
  return std::array{
      Opcode(0xEA, "NOP", "implied").Then(Internal(MicroInternalOp::kNone, Always(), "idle")).Build(),
  };
}

constexpr auto MakeStackSpecs() {
  return std::array{
      Opcode(0x48, "PHA", "implied").Then(PushAccumulator()).Build(),
      Opcode(0x8B, "PHB", "implied").Then(PushDataBank()).Build(),
      Opcode(0xAB, "PLB", "implied").Then(PullDataBank()).Build(),
  };
}

constexpr auto MakeLoadSpecs() {
  return std::array{
      Opcode(0xA9, "LDA", "immediate").Then(LoadAccumulatorImmediate()).Build(),
      Opcode(0xA2, "LDX", "immediate").Then(LoadIndexXImmediate()).Build(),
      Opcode(0xA0, "LDY", "immediate").Then(LoadIndexYImmediate()).Build(),
  };
}

constexpr auto MakeStoreSpecs() {
  return std::array{
      Opcode(0x8D, "STA", "absolute").Then(FetchAbsoluteAddr()).Then(StoreAccumulator()).Build(),
      Opcode(0x8E, "STX", "absolute").Then(FetchAbsoluteAddr()).Then(StoreIndexX()).Build(),
      Opcode(0x8C, "STY", "absolute").Then(FetchAbsoluteAddr()).Then(StoreIndexY()).Build(),
      Opcode(0x8F, "STA", "absolute long").Then(FetchLongAddr()).Then(StoreAccumulator()).Build(),
  };
}

constexpr auto MakeBranchSpecs() {
  return std::array{
      Opcode(0x80, "BRA", "relative").Then(BranchSequence(MicroInternalOp::kSetBranchTaken)).Build(),
      Opcode(0xD0, "BNE", "relative").Then(BranchSequence(MicroInternalOp::kSetBranchTakenIfNotZero)).Build(),
  };
}

constexpr auto kExplicitOpcodeSpecs = ConcatArrays(
    ConcatArrays(ConcatArrays(MakeMiscSpecs(), MakeLoadSpecs()), ConcatArrays(MakeStoreSpecs(), MakeBranchSpecs())),
    MakeStackSpecs());

static_assert(ValidateOpcodeSpecs(kExplicitOpcodeSpecs), "Opcode specification validation failed");

}  // namespace

const OpcodeArtifacts kOpcodeArtifacts = BuildOpcodeArtifacts(kExplicitOpcodeSpecs);

}  // namespace pupsnes::opcode_defs_internal

namespace pupsnes {
const std::array<InstructionEntry, 256> CPU::kOpcodeTable = opcode_defs_internal::kOpcodeArtifacts.execution_table;
}  // namespace pupsnes
