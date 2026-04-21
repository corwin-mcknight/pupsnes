#include "addressing_fragments.h"
#include "cpu_opcode_defs_internal.h"
#include "pupsnes/5a22/opcode_metadata.h"

namespace pupsnes::opcode_defs_internal {
namespace {

// FetchAddrByte, FetchAbsolute, and FetchAbsoluteLong were promoted to the
// public addressing_fragments.h header (D-01, D-02, D-04). Call sites in
// this file now resolve to those public definitions.

constexpr CycleFragment LoadAccumulatorImmediate() {
  return Fragment()
      .Then(LoadRegFromFetch(Reg::kA, ByteSel::kLow, true, Not(Condition(TimingCondition::kAccumulator16)),
                             "fetch immediate low"))
      .Then(LoadRegFromFetch(Reg::kA, ByteSel::kLow, false, Condition(TimingCondition::kAccumulator16),
                             "fetch immediate low"))
      .Then(LoadRegFromFetch(Reg::kA, ByteSel::kHigh, true, Condition(TimingCondition::kAccumulator16),
                             "fetch immediate high"))
      .Build();
}

constexpr CycleFragment LoadIndexXImmediate() {
  return Fragment()
      .Then(LoadRegFromFetch(Reg::kX, ByteSel::kLow, true, Not(Condition(TimingCondition::kIndex16)),
                             "fetch immediate low"))
      .Then(
          LoadRegFromFetch(Reg::kX, ByteSel::kLow, false, Condition(TimingCondition::kIndex16), "fetch immediate low"))
      .Then(
          LoadRegFromFetch(Reg::kX, ByteSel::kHigh, true, Condition(TimingCondition::kIndex16), "fetch immediate high"))
      .Build();
}

constexpr CycleFragment AluImmediateAccumulator(AluOp op) {
  // ALU immediate for A: 2 cycles if M=1, 3 cycles if M=0.
  return Fragment()
      .Then(AluImm8(op, Not(Condition(TimingCondition::kAccumulator16)), "fetch imm (8)"))
      .Then(FetchAddrByte(ByteSel::kLow, false, Condition(TimingCondition::kAccumulator16), "fetch imm low"))
      .Then(AluImm16(op, Condition(TimingCondition::kAccumulator16), "fetch imm high"))
      .Build();
}

constexpr CycleFragment AluImmediateIndex(AluOp op) {
  // ALU immediate keyed on X flag (for CPX/CPY): 2 cycles if X=1, 3 if X=0.
  return Fragment()
      .Then(AluImm8(op, Not(Condition(TimingCondition::kIndex16)), "fetch imm (8)"))
      .Then(FetchAddrByte(ByteSel::kLow, false, Condition(TimingCondition::kIndex16), "fetch imm low"))
      .Then(AluImm16(op, Condition(TimingCondition::kIndex16), "fetch imm high"))
      .Build();
}

constexpr CycleFragment LoadIndexYImmediate() {
  return Fragment()
      .Then(LoadRegFromFetch(Reg::kY, ByteSel::kLow, true, Not(Condition(TimingCondition::kIndex16)),
                             "fetch immediate low"))
      .Then(
          LoadRegFromFetch(Reg::kY, ByteSel::kLow, false, Condition(TimingCondition::kIndex16), "fetch immediate low"))
      .Then(
          LoadRegFromFetch(Reg::kY, ByteSel::kHigh, true, Condition(TimingCondition::kIndex16), "fetch immediate high"))
      .Build();
}

constexpr CycleFragment BranchSequence(BranchCond cond) {
  return Fragment()
      .Then(FetchPcBranchTest(cond))
      .Then(BranchRelative(false, Condition(TimingCondition::kBranchTaken), "apply branch"))
      .Then(Internal(MicroInternalOp::kNone,
                     AllOf(Condition(TimingCondition::kEmulationMode), Condition(TimingCondition::kBranchPageCrossed)),
                     "emulation page-cross penalty"))
      .Build();
}

// Note: StoreAccumulator / StoreIndexX / StoreIndexY pair WriteRegByte's bus
// action with a post-write kModifyAddr increment. Both ops share the same
// CycleSlotSpec::params byte — WriteRegByte's packing occupies bits [3:0] and
// kModifyAddr reads bit 4 (which is 0 in the shared encoding, meaning
// increment). This convention keeps both ops co-resident without adding a
// second params axis; see PackModify documentation.
constexpr CycleFragment StoreAccumulator() {
  return Fragment()
      .Then(WriteRegByte(WriteSrc::kA, ByteSel::kLow, MicroInternalOp::kModifyAddr, Always(), "write A low"))
      .Then(WriteRegByte(WriteSrc::kA, ByteSel::kHigh, MicroInternalOp::kNone,
                         Condition(TimingCondition::kAccumulator16), "write A high"))
      .Build();
}

constexpr CycleFragment StoreIndexX() {
  return Fragment()
      .Then(WriteRegByte(WriteSrc::kX, ByteSel::kLow, MicroInternalOp::kModifyAddr, Always(), "write X low"))
      .Then(WriteRegByte(WriteSrc::kX, ByteSel::kHigh, MicroInternalOp::kNone, Condition(TimingCondition::kIndex16),
                         "write X high"))
      .Build();
}

constexpr CycleFragment StoreIndexY() {
  return Fragment()
      .Then(WriteRegByte(WriteSrc::kY, ByteSel::kLow, MicroInternalOp::kModifyAddr, Always(), "write Y low"))
      .Then(WriteRegByte(WriteSrc::kY, ByteSel::kHigh, MicroInternalOp::kNone, Condition(TimingCondition::kIndex16),
                         "write Y high"))
      .Build();
}

constexpr CycleFragment PushAccumulator() {
  return Fragment()
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(PushReg(PushSrc::kAHigh, MicroInternalOp::kModifySp, Condition(TimingCondition::kAccumulator16),
                    "push A high"))
      .Then(PushReg(PushSrc::kA8, MicroInternalOp::kModifySp, Always(), "push A low"))
      .Build();
}

constexpr CycleFragment PushDataBank() {
  return Fragment()
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(PushReg(PushSrc::kDbr, MicroInternalOp::kModifySp, Always(), "push DBR"))
      .Build();
}

constexpr CycleFragment PullDataBank() {
  return Fragment()
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(ModifySp(true, Always(), "increment SP"))
      .Then(CycleSlotSpec{MicroBusAction::kPullStack, MicroInternalOp::kLoadReg, Always(), "pull DBR",
                          micro_op_params::PackLoadReg(Reg::kDbr, ByteSel::kLow, true)})
      .Build();
}

constexpr auto MakeMiscSpecs() {
  return std::array{
      Opcode(0xEA, "NOP", "implied").Then(Internal(MicroInternalOp::kNone, Always(), "idle")).Build(),
  };
}

// Push + decrement-SP shorthand: every real push cycle pairs the bus write
// with a post-write SP decrement, so this specializes PushReg() with the
// kModifySp internal op. The two ops share CycleSlotSpec::params: PushStack
// packs PushSrc into bits [3:0] (all current values fit in [3:0]); kModifySp
// reads bit 5 as "increment" and bit 5 is 0 in the shared encoding, meaning
// decrement — which is exactly what every push needs.
constexpr CycleSlotSpec PushRegSlot(PushSrc src, TimingRuleExpr rule, std::string_view label) {
  return PushReg(src, MicroInternalOp::kModifySp, rule, label);
}

constexpr CycleFragment PushIndexX() {
  return Fragment()
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(PushRegSlot(PushSrc::kXHigh, Condition(TimingCondition::kIndex16), "push X high"))
      .Then(PushRegSlot(PushSrc::kX8, Always(), "push X low"))
      .Build();
}

constexpr CycleFragment PushIndexY() {
  return Fragment()
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(PushRegSlot(PushSrc::kYHigh, Condition(TimingCondition::kIndex16), "push Y high"))
      .Then(PushRegSlot(PushSrc::kY8, Always(), "push Y low"))
      .Build();
}

constexpr CycleFragment PushStatus() {
  return Fragment()
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(PushRegSlot(PushSrc::kP, Always(), "push P"))
      .Build();
}

constexpr CycleFragment PushDirectPage() {
  // PHD: 4 cycles. Always 16-bit.
  return Fragment()
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(PushRegSlot(PushSrc::kDpHigh, Always(), "push DP high"))
      .Then(PushRegSlot(PushSrc::kDpLow, Always(), "push DP low"))
      .Build();
}

constexpr CycleFragment PushProgramBank() {
  return Fragment()
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(PushRegSlot(PushSrc::kPbr, Always(), "push PBR"))
      .Build();
}

constexpr CycleFragment PushEffectiveAbsolute() {
  // PEA #imm16: 5 cycles total. Fetch 2 bytes then push both (high first).
  return Fragment()
      .Then(FetchAddrByte(ByteSel::kLow, false, Always(), "fetch value low"))
      .Then(FetchAddrByte(ByteSel::kHigh, false, Always(), "fetch value high"))
      .Then(PushRegSlot(PushSrc::kAddrHigh, Always(), "push value high"))
      .Then(PushRegSlot(PushSrc::kAddrLow, Always(), "push value low"))
      .Build();
}

constexpr CycleFragment PullAccumulator() {
  return Fragment()
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(PullPreIncLoadReg(Reg::kA, ByteSel::kLow, true, Not(Condition(TimingCondition::kAccumulator16)),
                              "pull A (8-bit)"))
      .Then(PullPreIncLoadReg(Reg::kA, ByteSel::kLow, false, Condition(TimingCondition::kAccumulator16), "pull A low"))
      .Then(PullPreIncLoadReg(Reg::kA, ByteSel::kHigh, true, Condition(TimingCondition::kAccumulator16), "pull A high"))
      .Build();
}

constexpr CycleFragment PullIndexX() {
  return Fragment()
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(
          PullPreIncLoadReg(Reg::kX, ByteSel::kLow, true, Not(Condition(TimingCondition::kIndex16)), "pull X (8-bit)"))
      .Then(PullPreIncLoadReg(Reg::kX, ByteSel::kLow, false, Condition(TimingCondition::kIndex16), "pull X low"))
      .Then(PullPreIncLoadReg(Reg::kX, ByteSel::kHigh, true, Condition(TimingCondition::kIndex16), "pull X high"))
      .Build();
}

constexpr CycleFragment PullIndexY() {
  return Fragment()
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(
          PullPreIncLoadReg(Reg::kY, ByteSel::kLow, true, Not(Condition(TimingCondition::kIndex16)), "pull Y (8-bit)"))
      .Then(PullPreIncLoadReg(Reg::kY, ByteSel::kLow, false, Condition(TimingCondition::kIndex16), "pull Y low"))
      .Then(PullPreIncLoadReg(Reg::kY, ByteSel::kHigh, true, Condition(TimingCondition::kIndex16), "pull Y high"))
      .Build();
}

constexpr CycleFragment PullStatus() {
  return Fragment()
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(PullPreIncLoadReg(Reg::kP, ByteSel::kLow, false, Always(), "pull P"))
      .Build();
}

constexpr CycleFragment PullDirectPage() {
  // PLD: 5 cycles, always 16-bit.
  return Fragment()
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(PullPreIncLoadReg(Reg::kDp, ByteSel::kLow, false, Always(), "pull DP low"))
      .Then(PullPreIncLoadReg(Reg::kDp, ByteSel::kHigh, true, Always(), "pull DP high"))
      .Build();
}

constexpr auto MakeStackSpecs() {
  return std::array{
      Opcode(0x48, "PHA", "implied").Then(PushAccumulator()).Build(),
      Opcode(0x8B, "PHB", "implied").Then(PushDataBank()).Build(),
      Opcode(0xAB, "PLB", "implied").Then(PullDataBank()).Build(),
      Opcode(0xDA, "PHX", "implied").Then(PushIndexX()).Build(),
      Opcode(0x5A, "PHY", "implied").Then(PushIndexY()).Build(),
      Opcode(0x08, "PHP", "implied").Then(PushStatus()).Build(),
      Opcode(0x0B, "PHD", "implied").Then(PushDirectPage()).Build(),
      Opcode(0x4B, "PHK", "implied").Then(PushProgramBank()).Build(),
      Opcode(0x68, "PLA", "implied").Then(PullAccumulator()).Build(),
      Opcode(0xFA, "PLX", "implied").Then(PullIndexX()).Build(),
      Opcode(0x7A, "PLY", "implied").Then(PullIndexY()).Build(),
      Opcode(0x28, "PLP", "implied").Then(PullStatus()).Build(),
      Opcode(0x2B, "PLD", "implied").Then(PullDirectPage()).Build(),
      Opcode(0xF4, "PEA", "absolute").Then(PushEffectiveAbsolute()).Build(),
  };
}

constexpr auto MakeLoadSpecs() {
  return std::array{
      Opcode(0xA9, "LDA", "immediate").Then(LoadAccumulatorImmediate()).Build(),
      Opcode(0xA2, "LDX", "immediate index").Then(LoadIndexXImmediate()).Build(),
      Opcode(0xA0, "LDY", "immediate index").Then(LoadIndexYImmediate()).Build(),
      Opcode(0xA5, "LDA", "direct page")
          .Then(FetchDirectPage())
          .Then(LoadRegFromAddr(Reg::kA, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0xA6, "LDX", "direct page")
          .Then(FetchDirectPage())
          .Then(LoadRegFromAddr(Reg::kX, TimingCondition::kIndex16))
          .Build(),
      Opcode(0xA4, "LDY", "direct page")
          .Then(FetchDirectPage())
          .Then(LoadRegFromAddr(Reg::kY, TimingCondition::kIndex16))
          .Build(),
      Opcode(0xB5, "LDA", "direct page indexed X")
          .Then(FetchDirectPageIndexed(Reg::kX))
          .Then(LoadRegFromAddr(Reg::kA, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0xB4, "LDY", "direct page indexed X")
          .Then(FetchDirectPageIndexed(Reg::kX))
          .Then(LoadRegFromAddr(Reg::kY, TimingCondition::kIndex16))
          .Build(),
      Opcode(0xB6, "LDX", "direct page indexed Y")
          .Then(FetchDirectPageIndexed(Reg::kY))
          .Then(LoadRegFromAddr(Reg::kX, TimingCondition::kIndex16))
          .Build(),
      Opcode(0xA3, "LDA", "stack relative")
          .Then(FetchStackRelative())
          .Then(LoadRegFromAddr(Reg::kA, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0xB2, "LDA", "direct indirect")
          .Then(FetchDirectPage())
          .Then(FetchDirectIndirect())
          .Then(LoadRegFromAddr(Reg::kA, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0xA7, "LDA", "direct indirect long")
          .Then(FetchDirectPage())
          .Then(FetchDirectIndirectLong())
          .Then(LoadRegFromAddr(Reg::kA, TimingCondition::kAccumulator16))
          .Build(),
  };
}

constexpr auto MakeStoreSpecs() {
  return std::array{
      Opcode(0x8D, "STA", "absolute").Then(FetchAbsolute()).Then(StoreAccumulator()).Build(),
      Opcode(0x8E, "STX", "absolute").Then(FetchAbsolute()).Then(StoreIndexX()).Build(),
      Opcode(0x8C, "STY", "absolute").Then(FetchAbsolute()).Then(StoreIndexY()).Build(),
      Opcode(0x8F, "STA", "absolute long").Then(FetchAbsoluteLong()).Then(StoreAccumulator()).Build(),
      Opcode(0x85, "STA", "direct page")
          .Then(FetchDirectPage())
          .Then(StoreRegToAddr(WriteSrc::kA, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0x86, "STX", "direct page")
          .Then(FetchDirectPage())
          .Then(StoreRegToAddr(WriteSrc::kX, TimingCondition::kIndex16))
          .Build(),
      Opcode(0x84, "STY", "direct page")
          .Then(FetchDirectPage())
          .Then(StoreRegToAddr(WriteSrc::kY, TimingCondition::kIndex16))
          .Build(),
      Opcode(0x64, "STZ", "direct page")
          .Then(FetchDirectPage())
          .Then(StoreRegToAddr(WriteSrc::kZero, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0x95, "STA", "direct page indexed X")
          .Then(FetchDirectPageIndexed(Reg::kX))
          .Then(StoreRegToAddr(WriteSrc::kA, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0x94, "STY", "direct page indexed X")
          .Then(FetchDirectPageIndexed(Reg::kX))
          .Then(StoreRegToAddr(WriteSrc::kY, TimingCondition::kIndex16))
          .Build(),
      Opcode(0x96, "STX", "direct page indexed Y")
          .Then(FetchDirectPageIndexed(Reg::kY))
          .Then(StoreRegToAddr(WriteSrc::kX, TimingCondition::kIndex16))
          .Build(),
      Opcode(0x74, "STZ", "direct page indexed X")
          .Then(FetchDirectPageIndexed(Reg::kX))
          .Then(StoreRegToAddr(WriteSrc::kZero, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0x9C, "STZ", "absolute")
          .Then(FetchAbsolute())
          .Then(StoreRegToAddr(WriteSrc::kZero, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0x9E, "STZ", "absolute indexed X")
          .Then(FetchAbsoluteIndexed(Reg::kX))
          .Then(StoreRegToAddr(WriteSrc::kZero, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0x9D, "STA", "absolute indexed X")
          .Then(FetchAbsoluteIndexed(Reg::kX))
          .Then(StoreRegToAddr(WriteSrc::kA, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0x99, "STA", "absolute indexed Y")
          .Then(FetchAbsoluteIndexed(Reg::kY))
          .Then(StoreRegToAddr(WriteSrc::kA, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0x83, "STA", "stack relative")
          .Then(FetchStackRelative())
          .Then(StoreRegToAddr(WriteSrc::kA, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0x92, "STA", "direct indirect")
          .Then(FetchDirectPage())
          .Then(FetchDirectIndirect())
          .Then(StoreRegToAddr(WriteSrc::kA, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0x87, "STA", "direct indirect long")
          .Then(FetchDirectPage())
          .Then(FetchDirectIndirectLong())
          .Then(StoreRegToAddr(WriteSrc::kA, TimingCondition::kAccumulator16))
          .Build(),
  };
}

constexpr auto MakeIncDecSpecs() {
  return std::array{
      Opcode(0x1A, "INC", "implied").Then(IncDecReg(Reg::kA, false, Always(), "inc A")).Build(),
      Opcode(0x3A, "DEC", "implied").Then(IncDecReg(Reg::kA, true, Always(), "dec A")).Build(),
      Opcode(0xE8, "INX", "implied").Then(IncDecReg(Reg::kX, false, Always(), "inc X")).Build(),
      Opcode(0xC8, "INY", "implied").Then(IncDecReg(Reg::kY, false, Always(), "inc Y")).Build(),
      Opcode(0xCA, "DEX", "implied").Then(IncDecReg(Reg::kX, true, Always(), "dec X")).Build(),
      Opcode(0x88, "DEY", "implied").Then(IncDecReg(Reg::kY, true, Always(), "dec Y")).Build(),
  };
}

constexpr auto MakeFlagSpecs() {
  return std::array{
      Opcode(0x18, "CLC", "implied").Then(SetFlag(Flag::kC, false, Always(), "clear C")).Build(),
      Opcode(0x38, "SEC", "implied").Then(SetFlag(Flag::kC, true, Always(), "set C")).Build(),
      Opcode(0x58, "CLI", "implied").Then(SetFlag(Flag::kI, false, Always(), "clear I")).Build(),
      Opcode(0x78, "SEI", "implied").Then(SetFlag(Flag::kI, true, Always(), "set I")).Build(),
      Opcode(0xB8, "CLV", "implied").Then(SetFlag(Flag::kV, false, Always(), "clear V")).Build(),
      Opcode(0xD8, "CLD", "implied").Then(SetFlag(Flag::kD, false, Always(), "clear D")).Build(),
      Opcode(0xF8, "SED", "implied").Then(SetFlag(Flag::kD, true, Always(), "set D")).Build(),
      Opcode(0xFB, "XCE", "implied")
          .Then(Internal(MicroInternalOp::kExchangeCarryEmulation, Always(), "swap C and E"))
          .Build(),
      Opcode(0xC2, "REP", "immediate byte")
          .Then(FetchPc(MicroInternalOp::kNone, Always(), "fetch mask"))
          .Then(MaskStatus(false, Always(), "apply REP mask"))
          .Build(),
      Opcode(0xE2, "SEP", "immediate byte")
          .Then(FetchPc(MicroInternalOp::kNone, Always(), "fetch mask"))
          .Then(MaskStatus(true, Always(), "apply SEP mask"))
          .Build(),
  };
}

constexpr auto MakeTransferSpecs() {
  return std::array{
      Opcode(0xAA, "TAX", "implied").Then(TransferReg(Reg::kA, Reg::kX, Always(), "A -> X")).Build(),
      Opcode(0xA8, "TAY", "implied").Then(TransferReg(Reg::kA, Reg::kY, Always(), "A -> Y")).Build(),
      Opcode(0xBA, "TSX", "implied").Then(TransferReg(Reg::kSp, Reg::kX, Always(), "S -> X")).Build(),
      Opcode(0x8A, "TXA", "implied").Then(TransferReg(Reg::kX, Reg::kA, Always(), "X -> A")).Build(),
      Opcode(0x9A, "TXS", "implied").Then(TransferReg(Reg::kX, Reg::kSp, Always(), "X -> S")).Build(),
      Opcode(0x9B, "TXY", "implied").Then(TransferReg(Reg::kX, Reg::kY, Always(), "X -> Y")).Build(),
      Opcode(0x98, "TYA", "implied").Then(TransferReg(Reg::kY, Reg::kA, Always(), "Y -> A")).Build(),
      Opcode(0xBB, "TYX", "implied").Then(TransferReg(Reg::kY, Reg::kX, Always(), "Y -> X")).Build(),
      Opcode(0x5B, "TCD", "implied").Then(TransferReg(Reg::kA, Reg::kDp, Always(), "C -> D")).Build(),
      Opcode(0x1B, "TCS", "implied").Then(TransferReg(Reg::kA, Reg::kSp, Always(), "C -> S")).Build(),
      Opcode(0x7B, "TDC", "implied").Then(TransferReg(Reg::kDp, Reg::kA, Always(), "D -> C")).Build(),
      Opcode(0x3B, "TSC", "implied").Then(TransferReg(Reg::kSp, Reg::kA, Always(), "S -> C")).Build(),
  };
}

constexpr CycleFragment BranchLongSequence() {
  // BRL: fetch 16-bit signed displacement into addr_, then apply relative jump.
  return Fragment()
      .Then(FetchAddrByte(ByteSel::kLow, false, Always(), "fetch disp low"))
      .Then(FetchAddrByte(ByteSel::kHigh, false, Always(), "fetch disp high"))
      .Then(BranchRelative(true, Always(), "apply long branch"))
      .Build();
}

constexpr CycleFragment JumpAbsolute() {
  // JMP abs: 3 cycles total. Opcode fetch + fetch low + fetch high (which also
  // sets PC).
  return Fragment()
      .Then(FetchAddrByte(ByteSel::kLow, false, Always(), "fetch target low"))
      .Then(LoadAddrByteAndSetPc(ByteSel::kHigh, false, Always(), "fetch target high, set PC"))
      .Build();
}

constexpr CycleFragment JumpAbsoluteLong() {
  // JMP long: 4 cycles total. Opcode fetch + low + high + bank (sets PC+PBR).
  return Fragment()
      .Then(FetchAddrByte(ByteSel::kLow, false, Always(), "fetch target low"))
      .Then(FetchAddrByte(ByteSel::kHigh, false, Always(), "fetch target high"))
      .Then(LoadAddrByteAndSetPc(ByteSel::kBank, true, Always(), "fetch bank, set PC+PBR"))
      .Build();
}

constexpr auto MakeBranchSpecs() {
  return std::array{
      Opcode(0x80, "BRA", "relative").Then(BranchSequence(BranchCond::kAlways)).Build(),
      Opcode(0xD0, "BNE", "relative").Then(BranchSequence(BranchCond::kNotZ)).Build(),
      Opcode(0xF0, "BEQ", "relative").Then(BranchSequence(BranchCond::kZ)).Build(),
      Opcode(0x90, "BCC", "relative").Then(BranchSequence(BranchCond::kNotC)).Build(),
      Opcode(0xB0, "BCS", "relative").Then(BranchSequence(BranchCond::kC)).Build(),
      Opcode(0x10, "BPL", "relative").Then(BranchSequence(BranchCond::kNotN)).Build(),
      Opcode(0x30, "BMI", "relative").Then(BranchSequence(BranchCond::kN)).Build(),
      Opcode(0x50, "BVC", "relative").Then(BranchSequence(BranchCond::kNotV)).Build(),
      Opcode(0x70, "BVS", "relative").Then(BranchSequence(BranchCond::kV)).Build(),
      Opcode(0x82, "BRL", "relative long").Then(BranchLongSequence()).Build(),
  };
}

constexpr CycleFragment JsrAbsolute() {
  // JSR abs: 6 cycles total. Push PC+2 (the address of the final JSR byte),
  // then jump to the fetched target.
  // After opcode fetch (cycle 1, auto) PC = JSR+1.
  // T2: fetch target low, PC = JSR+2.
  // T3: internal. PC stays at JSR+2 (this is what gets pushed).
  // T4: push PCH.
  // T5: push PCL, decrement SP.
  // T6: fetch target high, set PC.
  return Fragment()
      .Then(FetchAddrByte(ByteSel::kLow, false, Always(), "fetch target low"))
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(PushRegSlot(PushSrc::kPch, Always(), "push PCH"))
      .Then(PushRegSlot(PushSrc::kPcl, Always(), "push PCL"))
      .Then(LoadAddrByteAndSetPc(ByteSel::kHigh, false, Always(), "fetch target high, set PC"))
      .Build();
}

constexpr CycleFragment JsrAbsoluteLong() {
  // JSL long: 8 cycles total. Push PBR, push PC+3, jump to 24-bit target.
  // After opcode fetch PC = JSR+1.
  // T2: fetch target low, PC = JSR+2.
  // T3: fetch target high, PC = JSR+3.
  // T4: push PBR (we will overwrite PBR in the final step).
  // T5: internal.
  // T6: push PCH (PC = JSR+3, one less than next instruction JSR+4).
  // T7: push PCL.
  // T8: fetch target bank, set PC+PBR.
  return Fragment()
      .Then(FetchAddrByte(ByteSel::kLow, false, Always(), "fetch target low"))
      .Then(FetchAddrByte(ByteSel::kHigh, false, Always(), "fetch target high"))
      .Then(PushRegSlot(PushSrc::kPbr, Always(), "push PBR"))
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(PushRegSlot(PushSrc::kPch, Always(), "push PCH"))
      .Then(PushRegSlot(PushSrc::kPcl, Always(), "push PCL"))
      .Then(LoadAddrByteAndSetPc(ByteSel::kBank, true, Always(), "fetch bank, set PC+PBR"))
      .Build();
}

// Build a "pre-increment-pull + LoadReg(reg=PCL/PCH/PBR)" slot. Used by
// RTS/RTL to pull the return address bytes directly into PC.
constexpr CycleSlotSpec PullPreIncLoadPcByte(Reg reg, std::string_view label) {
  return CycleSlotSpec{
      MicroBusAction::kPreIncPullStack,
      MicroInternalOp::kLoadReg,
      Always(),
      label,
      micro_op_params::PackLoadReg(reg, ByteSel::kLow, false),
  };
}

constexpr CycleFragment Rts() {
  // RTS: 6 cycles total. Pull PCL, pull PCH, PC += 1.
  return Fragment()
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(PullPreIncLoadPcByte(Reg::kPcl, "pull PCL"))
      .Then(PullPreIncLoadPcByte(Reg::kPch, "pull PCH"))
      .Then(ModifyPc(true, Always(), "increment PC"))
      .Build();
}

constexpr CycleFragment Rtl() {
  // RTL: 6 cycles total. Pull PCL, pull PCH, increment PC, pull PBR.
  return Fragment()
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal"))
      .Then(PullPreIncLoadPcByte(Reg::kPcl, "pull PCL"))
      .Then(PullPreIncLoadPcByte(Reg::kPch, "pull PCH"))
      .Then(ModifyPc(true, Always(), "increment PC"))
      .Then(PullPreIncLoadPcByte(Reg::kPbr, "pull PBR"))
      .Build();
}

constexpr CycleSlotSpec ShiftRotateAccumulator(ShiftOp op, std::string_view label) {
  return CycleSlotSpec{
      MicroBusAction::kNone,
      MicroInternalOp::kShiftRotateA,
      Always(),
      label,
      static_cast<uint8_t>(static_cast<uint32_t>(op) & 0x03U),
  };
}

constexpr auto MakeShiftSpecs() {
  return std::array{
      Opcode(0x0A, "ASL", "implied").Then(ShiftRotateAccumulator(ShiftOp::kAsl, "ASL A")).Build(),
      Opcode(0x4A, "LSR", "implied").Then(ShiftRotateAccumulator(ShiftOp::kLsr, "LSR A")).Build(),
      Opcode(0x2A, "ROL", "implied").Then(ShiftRotateAccumulator(ShiftOp::kRol, "ROL A")).Build(),
      Opcode(0x6A, "ROR", "implied").Then(ShiftRotateAccumulator(ShiftOp::kRor, "ROR A")).Build(),
  };
}

constexpr auto MakeAluSpecs() {
  return std::array{
      Opcode(0x69, "ADC", "immediate").Then(AluImmediateAccumulator(AluOp::kAdc)).Build(),
      Opcode(0xE9, "SBC", "immediate").Then(AluImmediateAccumulator(AluOp::kSbc)).Build(),
      Opcode(0x29, "AND", "immediate").Then(AluImmediateAccumulator(AluOp::kAnd)).Build(),
      Opcode(0x09, "ORA", "immediate").Then(AluImmediateAccumulator(AluOp::kOra)).Build(),
      Opcode(0x49, "EOR", "immediate").Then(AluImmediateAccumulator(AluOp::kEor)).Build(),
      Opcode(0xC9, "CMP", "immediate").Then(AluImmediateAccumulator(AluOp::kCmp)).Build(),
      Opcode(0x89, "BIT", "immediate").Then(AluImmediateAccumulator(AluOp::kBit)).Build(),
      Opcode(0xE0, "CPX", "immediate index").Then(AluImmediateIndex(AluOp::kCpx)).Build(),
      Opcode(0xC0, "CPY", "immediate index").Then(AluImmediateIndex(AluOp::kCpy)).Build(),
      Opcode(0x65, "ADC", "direct page")
          .Then(FetchDirectPage())
          .Then(AluFromAddr(AluOp::kAdc, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0xE5, "SBC", "direct page")
          .Then(FetchDirectPage())
          .Then(AluFromAddr(AluOp::kSbc, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0x25, "AND", "direct page")
          .Then(FetchDirectPage())
          .Then(AluFromAddr(AluOp::kAnd, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0x05, "ORA", "direct page")
          .Then(FetchDirectPage())
          .Then(AluFromAddr(AluOp::kOra, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0x45, "EOR", "direct page")
          .Then(FetchDirectPage())
          .Then(AluFromAddr(AluOp::kEor, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0xC5, "CMP", "direct page")
          .Then(FetchDirectPage())
          .Then(AluFromAddr(AluOp::kCmp, TimingCondition::kAccumulator16))
          .Build(),
  };
}

constexpr auto MakeJumpSpecs() {
  return std::array{
      Opcode(0x4C, "JMP", "absolute").Then(JumpAbsolute()).Build(),
      Opcode(0x5C, "JMP", "absolute long").Then(JumpAbsoluteLong()).Build(),
      Opcode(0x20, "JSR", "absolute").Then(JsrAbsolute()).Build(),
      Opcode(0x22, "JSL", "absolute long").Then(JsrAbsoluteLong()).Build(),
      Opcode(0x60, "RTS", "implied").Then(Rts()).Build(),
      Opcode(0x6B, "RTL", "implied").Then(Rtl()).Build(),
  };
}

constexpr auto MakeAluAbsSpecs() {
  return std::array{
      // ALU absolute (kAccumulator16 gating for M flag) — 6 ALU mnemonics
      Opcode(0x6D, "ADC", "absolute")
          .Then(FetchAbsolute())
          .Then(AluFromAddr(AluOp::kAdc, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0xED, "SBC", "absolute")
          .Then(FetchAbsolute())
          .Then(AluFromAddr(AluOp::kSbc, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0x2D, "AND", "absolute")
          .Then(FetchAbsolute())
          .Then(AluFromAddr(AluOp::kAnd, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0x0D, "ORA", "absolute")
          .Then(FetchAbsolute())
          .Then(AluFromAddr(AluOp::kOra, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0x4D, "EOR", "absolute")
          .Then(FetchAbsolute())
          .Then(AluFromAddr(AluOp::kEor, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0xCD, "CMP", "absolute")
          .Then(FetchAbsolute())
          .Then(AluFromAddr(AluOp::kCmp, TimingCondition::kAccumulator16))
          .Build(),
      // BIT abs — memory BIT: uses kBitMem for correct N/V/Z semantics (Bruce Clark §6.1.2.2).
      Opcode(0x2C, "BIT", "absolute")
          .Then(FetchAbsolute())
          .Then(AluFromAddr(AluOp::kBitMem, TimingCondition::kAccumulator16))
          .Build(),
      // ALU absolute long (kAccumulator16) — 6 ALU mnemonics (no BIT long per Bruce Clark).
      Opcode(0x6F, "ADC", "absolute long")
          .Then(FetchAbsoluteLong())
          .Then(AluFromAddr(AluOp::kAdc, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0xEF, "SBC", "absolute long")
          .Then(FetchAbsoluteLong())
          .Then(AluFromAddr(AluOp::kSbc, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0x2F, "AND", "absolute long")
          .Then(FetchAbsoluteLong())
          .Then(AluFromAddr(AluOp::kAnd, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0x0F, "ORA", "absolute long")
          .Then(FetchAbsoluteLong())
          .Then(AluFromAddr(AluOp::kOra, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0x4F, "EOR", "absolute long")
          .Then(FetchAbsoluteLong())
          .Then(AluFromAddr(AluOp::kEor, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0xCF, "CMP", "absolute long")
          .Then(FetchAbsoluteLong())
          .Then(AluFromAddr(AluOp::kCmp, TimingCondition::kAccumulator16))
          .Build(),
      // Load absolute (A: kAccumulator16; X/Y: kIndex16).
      Opcode(0xAD, "LDA", "absolute")
          .Then(FetchAbsolute())
          .Then(LoadRegFromAddr(Reg::kA, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0xAF, "LDA", "absolute long")
          .Then(FetchAbsoluteLong())
          .Then(LoadRegFromAddr(Reg::kA, TimingCondition::kAccumulator16))
          .Build(),
      Opcode(0xAE, "LDX", "absolute")
          .Then(FetchAbsolute())
          .Then(LoadRegFromAddr(Reg::kX, TimingCondition::kIndex16))
          .Build(),
      Opcode(0xAC, "LDY", "absolute")
          .Then(FetchAbsolute())
          .Then(LoadRegFromAddr(Reg::kY, TimingCondition::kIndex16))
          .Build(),
      // Compare absolute (CPX/CPY: kIndex16 per D-07).
      Opcode(0xEC, "CPX", "absolute")
          .Then(FetchAbsolute())
          .Then(AluFromAddr(AluOp::kCpx, TimingCondition::kIndex16))
          .Build(),
      Opcode(0xCC, "CPY", "absolute")
          .Then(FetchAbsolute())
          .Then(AluFromAddr(AluOp::kCpy, TimingCondition::kIndex16))
          .Build(),
  };
}

// BIT dp,X (0x34) — registered here alongside other kBitMem opcodes. kBitMem
// is defined in this plan (01-02), so this belongs with MakeAluAbsSpecs. The
// dp,X tests live in the ALU dp,X test file (plan 01-03). Cycle formula
// 5-m+w is identical to ADC dp,X per Bruce Clark §6.1.2.2 / §6.1.1.1.
constexpr auto MakeBitDpxSpec() {
  return std::array{
      Opcode(0x34, "BIT", "direct page indexed X")
          .Then(FetchDirectPageIndexed(Reg::kX))
          .Then(AluFromAddr(AluOp::kBitMem, TimingCondition::kAccumulator16))
          .Build(),
  };
}

constexpr auto kExplicitOpcodeSpecs = ConcatArrays(
    ConcatArrays(
        ConcatArrays(ConcatArrays(MakeMiscSpecs(), MakeLoadSpecs()), ConcatArrays(MakeStoreSpecs(), MakeBranchSpecs())),
        ConcatArrays(
            ConcatArrays(ConcatArrays(ConcatArrays(ConcatArrays(MakeStackSpecs(), MakeIncDecSpecs()), MakeFlagSpecs()),
                                      MakeTransferSpecs()),
                         MakeJumpSpecs()),
            ConcatArrays(MakeAluSpecs(), MakeShiftSpecs()))),
    ConcatArrays(MakeAluAbsSpecs(), MakeBitDpxSpec()));

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
  if (mode == "immediate byte") {
    return OpcodeAddressingMode::kImmediateByte;
  }
  if (mode == "relative long") {
    return OpcodeAddressingMode::kRelative16;
  }
  if (mode == "direct page") {
    return OpcodeAddressingMode::kDirectPage;
  }
  if (mode == "direct page indexed X") {
    return OpcodeAddressingMode::kDirectPageIndexedX;
  }
  if (mode == "direct page indexed Y") {
    return OpcodeAddressingMode::kDirectPageIndexedY;
  }
  if (mode == "stack relative") {
    return OpcodeAddressingMode::kStackRelative;
  }
  if (mode == "absolute indexed X") {
    return OpcodeAddressingMode::kAbsoluteIndexedX;
  }
  if (mode == "absolute indexed Y") {
    return OpcodeAddressingMode::kAbsoluteIndexedY;
  }
  if (mode == "direct indirect") {
    return OpcodeAddressingMode::kDirectIndirect;
  }
  if (mode == "direct indirect long") {
    return OpcodeAddressingMode::kDirectIndirectLong;
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
    case OpcodeAddressingMode::kUnknown: view.base_length = 1; break;
    case OpcodeAddressingMode::kImmediateAccumulator:
      view.base_length = 2;
      view.accumulator_width_dependent = true;
      break;
    case OpcodeAddressingMode::kImmediateIndex:
      view.base_length = 2;
      view.index_width_dependent = true;
      break;
    case OpcodeAddressingMode::kAbsolute: view.base_length = 3; break;
    case OpcodeAddressingMode::kAbsoluteLong: view.base_length = 4; break;
    case OpcodeAddressingMode::kRelative8:
    case OpcodeAddressingMode::kImmediateByte: view.base_length = 2; break;
    case OpcodeAddressingMode::kRelative16: view.base_length = 3; break;
    case OpcodeAddressingMode::kDirectPage:
    case OpcodeAddressingMode::kDirectPageIndexedX:
    case OpcodeAddressingMode::kDirectPageIndexedY:
    case OpcodeAddressingMode::kStackRelative: view.base_length = 2; break;
    case OpcodeAddressingMode::kAbsoluteIndexedX:
    case OpcodeAddressingMode::kAbsoluteIndexedY: view.base_length = 3; break;
    case OpcodeAddressingMode::kDirectIndirect:
    case OpcodeAddressingMode::kDirectIndirectLong: view.base_length = 2; break;
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
    case OpcodeAddressingMode::kUnknown: return "unknown";
    case OpcodeAddressingMode::kImplied: return "implied";
    case OpcodeAddressingMode::kImmediateAccumulator: return "immediate";
    case OpcodeAddressingMode::kImmediateIndex: return "immediate index";
    case OpcodeAddressingMode::kAbsolute: return "absolute";
    case OpcodeAddressingMode::kAbsoluteLong: return "absolute long";
    case OpcodeAddressingMode::kRelative8: return "relative";
    case OpcodeAddressingMode::kImmediateByte: return "immediate byte";
    case OpcodeAddressingMode::kRelative16: return "relative long";
    case OpcodeAddressingMode::kDirectPage: return "direct page";
    case OpcodeAddressingMode::kDirectPageIndexedX: return "direct page indexed X";
    case OpcodeAddressingMode::kDirectPageIndexedY: return "direct page indexed Y";
    case OpcodeAddressingMode::kStackRelative: return "stack relative";
    case OpcodeAddressingMode::kAbsoluteIndexedX: return "absolute indexed X";
    case OpcodeAddressingMode::kAbsoluteIndexedY: return "absolute indexed Y";
    case OpcodeAddressingMode::kDirectIndirect: return "direct indirect";
    case OpcodeAddressingMode::kDirectIndirectLong: return "direct indirect long";
  }
  return "unknown";
}
}  // namespace pupsnes
