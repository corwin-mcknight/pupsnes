#include "cpu_opcode_defs_internal.h"
#include "pupsnes/5a22/opcode_metadata.h"

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
      Opcode(0xA2, "LDX", "immediate index").Then(LoadIndexXImmediate()).Build(),
      Opcode(0xA0, "LDY", "immediate index").Then(LoadIndexYImmediate()).Build(),
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

constexpr auto MakeIncDecSpecs() {
  return std::array{
      Opcode(0x1A, "INC", "implied").Then(Internal(MicroInternalOp::kIncA, Always(), "inc A")).Build(),
      Opcode(0x3A, "DEC", "implied").Then(Internal(MicroInternalOp::kDecA, Always(), "dec A")).Build(),
      Opcode(0xE8, "INX", "implied").Then(Internal(MicroInternalOp::kIncX, Always(), "inc X")).Build(),
      Opcode(0xC8, "INY", "implied").Then(Internal(MicroInternalOp::kIncY, Always(), "inc Y")).Build(),
      Opcode(0xCA, "DEX", "implied").Then(Internal(MicroInternalOp::kDecX, Always(), "dec X")).Build(),
      Opcode(0x88, "DEY", "implied").Then(Internal(MicroInternalOp::kDecY, Always(), "dec Y")).Build(),
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
    ConcatArrays(MakeStackSpecs(), MakeIncDecSpecs()));

static_assert(ValidateOpcodeSpecs(kExplicitOpcodeSpecs), "Opcode specification validation failed");

}  // namespace

const OpcodeArtifacts kOpcodeArtifacts = BuildOpcodeArtifacts(kExplicitOpcodeSpecs);

}  // namespace pupsnes::opcode_defs_internal

namespace pupsnes {

namespace {

constexpr OpcodeAddressingMode MapAddressingMode(std::string_view mode) {
  if (mode == "implied") {
    return OpcodeAddressingMode::kImplied;
  }
  if (mode == "immediate") {
    return OpcodeAddressingMode::kImmediateAccumulator;
  }
  if (mode == "immediate index") {
    return OpcodeAddressingMode::kImmediateIndex;
  }
  if (mode == "absolute") {
    return OpcodeAddressingMode::kAbsolute;
  }
  if (mode == "absolute long") {
    return OpcodeAddressingMode::kAbsoluteLong;
  }
  if (mode == "relative") {
    return OpcodeAddressingMode::kRelative8;
  }
  return OpcodeAddressingMode::kUnknown;
}

constexpr OpcodeMetadataView LowerPublicMetadata(const opcode_defs_internal::OpcodeMetadata& metadata) {
  OpcodeMetadataView view{};
  view.mnemonic = metadata.mnemonic;
  view.implementation_status =
      metadata.implemented ? OpcodeImplementationStatus::kImplemented : OpcodeImplementationStatus::kUnimplemented;
  view.addressing_mode = MapAddressingMode(metadata.addressing_mode);

  switch (view.addressing_mode) {
    case OpcodeAddressingMode::kImplied:
    case OpcodeAddressingMode::kUnknown:
      view.base_length = 1;
      break;
    case OpcodeAddressingMode::kImmediateAccumulator:
      view.base_length = 2;
      view.accumulator_width_dependent = true;
      break;
    case OpcodeAddressingMode::kImmediateIndex:
      view.base_length = 2;
      view.index_width_dependent = true;
      break;
    case OpcodeAddressingMode::kAbsolute:
      view.base_length = 3;
      break;
    case OpcodeAddressingMode::kAbsoluteLong:
      view.base_length = 4;
      break;
    case OpcodeAddressingMode::kRelative8:
      view.base_length = 2;
      break;
  }

  return view;
}

std::array<OpcodeMetadataView, 256> BuildPublicMetadataTable() {
  std::array<OpcodeMetadataView, 256> table{};
  for (std::size_t i = 0; i < table.size(); ++i) {
    table[i] = LowerPublicMetadata(opcode_defs_internal::kOpcodeArtifacts.metadata_table[i]);
  }
  return table;
}

}  // namespace

const std::array<InstructionEntry, 256> CPU::kOpcodeTable = opcode_defs_internal::kOpcodeArtifacts.execution_table;

const std::array<OpcodeMetadataView, 256>& GetOpcodeMetadataTable() {
  static const std::array<OpcodeMetadataView, 256> kPublicOpcodeMetadata = BuildPublicMetadataTable();
  return kPublicOpcodeMetadata;
}

const OpcodeMetadataView& GetOpcodeMetadata(uint8_t opcode) { return GetOpcodeMetadataTable()[opcode]; }

uint8_t ComputeInstructionLength(const OpcodeMetadataView& metadata, const CpuFlags& flags) {
  uint8_t length = metadata.base_length;
  if (metadata.accumulator_width_dependent && !flags.E && !flags.M) {
    length = static_cast<uint8_t>(length + 1U);
  }
  if (metadata.index_width_dependent && !flags.E && !flags.X) {
    length = static_cast<uint8_t>(length + 1U);
  }
  return length;
}

std::string_view GetAddressingModeName(OpcodeAddressingMode mode) {
  switch (mode) {
    case OpcodeAddressingMode::kUnknown:
      return "unknown";
    case OpcodeAddressingMode::kImplied:
      return "implied";
    case OpcodeAddressingMode::kImmediateAccumulator:
      return "immediate";
    case OpcodeAddressingMode::kImmediateIndex:
      return "immediate index";
    case OpcodeAddressingMode::kAbsolute:
      return "absolute";
    case OpcodeAddressingMode::kAbsoluteLong:
      return "absolute long";
    case OpcodeAddressingMode::kRelative8:
      return "relative";
  }
  return "unknown";
}
}  // namespace pupsnes
