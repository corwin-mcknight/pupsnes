#pragma once

// Reusable CycleFragment helpers for 65C816 addressing modes. Each helper
// returns a CycleFragment that encodes the cycles needed to compute an
// effective address into addr_, or to read/write a register through addr_,
// honoring width flags (M / X) and per-mode timing penalties (e.g. "+w" when
// the DP low byte is nonzero).
//
// Fragments here are composed with OpcodeSpecBuilder in cpu_opcodes.cpp the
// same way FetchAbsolute / FetchAbsoluteLong / StoreAccumulator are today —
// see cpu_opcodes.cpp for the original patterns.

#include "pupsnes/5a22/cpu_opcode_defs_internal.h"

namespace pupsnes::opcode_defs_internal {

// ---------------------------------------------------------------------------
// Address computation
// ---------------------------------------------------------------------------

// Promoted from cpu_opcodes.cpp anonymous namespace (D-01, D-02).
// Byte-fetch primitive shared by FetchAbsolute, FetchAbsoluteLong, and
// FetchAbsoluteIndexed. Builds one cycle slot: FetchPc bus action +
// kSetAddrByteFromFetch internal op targeting byte_sel of addr_.
constexpr CycleSlotSpec FetchAddrByte(ByteSel byte_sel, bool from_dbr,
                                       TimingRuleExpr rule, std::string_view label) {
  return CycleSlotSpec{
      MicroBusAction::kFetchPc,
      MicroInternalOp::kSetAddrByteFromFetch,
      rule,
      label,
      micro_op_params::PackSetAddrByte(byte_sel, from_dbr),
  };
}

// Absolute-long effective address (3-byte operand). Fetches low, high, and
// bank bytes from PC, assembling a 24-bit address in addr_. No DBR used.
// Promoted from cpu_opcodes.cpp (D-01). Two fixed cycles + bank byte cycle.
constexpr CycleFragment FetchAbsoluteLong() {
  return Fragment()
      .Then(FetchAddrByte(ByteSel::kLow, false, Always(), "fetch address low"))
      .Then(FetchAddrByte(ByteSel::kHigh, false, Always(), "fetch address high"))
      .Then(FetchAddrByte(ByteSel::kBank, false, Always(), "fetch address bank"))
      .Build();
}

// Absolute effective address (2-byte operand). Fetches low and high bytes
// from PC; bank comes from DBR (from_dbr=true on the high byte). Promoted
// from cpu_opcodes.cpp (D-01).
constexpr CycleFragment FetchAbsolute() {
  return Fragment()
      .Then(FetchAddrByte(ByteSel::kLow, false, Always(), "fetch address low"))
      .Then(FetchAddrByte(ByteSel::kHigh, true, Always(), "fetch address high"))
      .Build();
}

// Direct-page effective address: fetch the 1-byte DP offset from PC and
// compute addr_ = bank 0, (DP + offset) & 0xFFFF in a single slot. The
// optional "+w" penalty cycle (DP low byte nonzero) is appended as a separate
// conditional internal slot — instructions that follow DP must include it.
//
// Cycle accounting: 1 fixed cycle (fetch + address compute) + 1 conditional
// cycle (DL != 0). Callers: LDA/STA/etc. in direct-page mode.
constexpr CycleFragment FetchDirectPage() {
  return Fragment()
      .Then(CycleSlotSpec{
          MicroBusAction::kFetchPc,
          MicroInternalOp::kSetAddrFromDp,
          Always(),
          "fetch DP offset, compute addr",
          0,
      })
      .Then(
          Internal(MicroInternalOp::kNone, Condition(TimingCondition::kDirectPageLowNonzero), "DP-low-nonzero penalty"))
      .Build();
}

// Direct-page indexed effective address: dp + (X or Y). Fetches the DP
// offset, computes (DP + offset), adds the chosen index (X or Y) — always
// bank 0 — and appends the DL-nonzero penalty slot. Two fixed cycles plus
// one conditional; the index-add is always present (that's the "+1" that
// distinguishes 5-m+w from 4-m+w). index_reg must be Reg::kX or Reg::kY.
constexpr CycleFragment FetchDirectPageIndexed(Reg index_reg) {
  return Fragment()
      .Then(CycleSlotSpec{
          MicroBusAction::kFetchPc,
          MicroInternalOp::kSetAddrFromDp,
          Always(),
          "fetch DP offset, compute addr",
          0,
      })
      .Then(
          Internal(MicroInternalOp::kNone, Condition(TimingCondition::kDirectPageLowNonzero), "DP-low-nonzero penalty"))
      .Then(CycleSlotSpec{
          MicroBusAction::kNone,
          MicroInternalOp::kAddIndexToAddr,
          Always(),
          "add index",
          micro_op_params::PackAddIndex(index_reg),
      })
      .Build();
}

// Absolute-indexed effective address: fetch the 2-byte absolute operand (with
// bank from DBR), then add the chosen index register. The index-add is
// unconditional (always 1 cycle) — this matches the 65C816's behavior for
// indexed *stores*, which pay the extra cycle regardless of page crossing.
// Three fixed cycles. index_reg must be Reg::kX or Reg::kY.
//
// Refactored (D-03) to call the promoted FetchAddrByte helper above instead
// of inlining CycleSlotSpec+PackSetAddrByte constructors for the first two
// slots. The third slot (index-add) remains inline.
constexpr CycleFragment FetchAbsoluteIndexed(Reg index_reg) {
  return Fragment()
      .Then(FetchAddrByte(ByteSel::kLow, false, Always(), "fetch address low"))
      .Then(FetchAddrByte(ByteSel::kHigh, true, Always(), "fetch address high"))
      .Then(CycleSlotSpec{
          MicroBusAction::kNone,
          MicroInternalOp::kAddIndexToAddr,
          Always(),
          "add index",
          micro_op_params::PackAddIndex(index_reg, /*bank_wrap=*/false),
      })
      .Build();
}

// Stack-relative effective address: addr_ = bank 0, (SP + offset) & 0xFFFF.
// Two cycles: fetch offset + compute, then an internal "add" cycle. Used by
// sr,S addressing (e.g. LDA $nn,S). No DL penalty because no DP math.
constexpr CycleFragment FetchStackRelative() {
  return Fragment()
      .Then(CycleSlotSpec{
          MicroBusAction::kFetchPc,
          MicroInternalOp::kSetAddrFromSp,
          Always(),
          "fetch SR offset, compute addr",
          0,
      })
      .Then(Internal(MicroInternalOp::kNone, Always(), "internal add"))
      .Build();
}

// Absolute-long indexed effective address: fetch 3-byte operand (low, high,
// bank) and add X to the low 16 bits with 24-bit carry into the bank byte.
// Four fixed cycles (no page-cross penalty — bank is explicit). Used by long,X
// addressing (e.g. LDA $FEDCBA,X). Cycle formula 6-m per Bruce Clark §6.1.1.1
// (6 at m=1 for reads/writes, plus one more cycle for m=0 read/write of the
// high byte through LoadRegFromAddr / StoreRegToAddr / AluFromAddr).
constexpr CycleFragment FetchAbsoluteLongIndexedX() {
  return Fragment()
      .Then(FetchAddrByte(ByteSel::kLow, false, Always(), "fetch address low"))
      .Then(FetchAddrByte(ByteSel::kHigh, false, Always(), "fetch address high"))
      .Then(FetchAddrByte(ByteSel::kBank, false, Always(), "fetch address bank"))
      .Then(CycleSlotSpec{
          MicroBusAction::kNone,
          MicroInternalOp::kAddIndexToAddr,
          Always(),
          "add X to addr",
          micro_op_params::PackAddIndex(Reg::kX, /*bank_wrap=*/false),
      })
      .Build();
}

// Direct indirect: read a 2-byte pointer from bank 0 at the DP-derived addr,
// assemble into DBR:(high:low). Leaves addr_ at the effective operand address.
// Two additional cycles on top of FetchDirectPage (which must be Then()'d
// first). Used by (dp) addressing.
constexpr CycleFragment FetchDirectIndirect() {
  return Fragment()
      .Then(
          CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kStashIndirectLow, Always(), "read pointer low", 0})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kFormAddrFromScratchDbr, Always(),
                          "read pointer high, assemble", 0})
      .Build();
}

// Direct indirect long: read a 3-byte pointer from bank 0 and assemble into
// bank:(high:low) using the bank byte from memory (not DBR). Three additional
// cycles on top of FetchDirectPage. Used by [dp] addressing.
constexpr CycleFragment FetchDirectIndirectLong() {
  return Fragment()
      .Then(
          CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kStashIndirectLow, Always(), "read pointer low", 0})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kStashIndirectHigh, Always(), "read pointer high",
                          0})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kFormAddrFromScratchBank, Always(),
                          "read pointer bank, assemble", 0})
      .Build();
}

// ---------------------------------------------------------------------------
// Operand access through addr_
// ---------------------------------------------------------------------------

// Load a register (A/X/Y) from addr_, respecting the appropriate width flag.
// Emits 1 cycle for the 8-bit-flag path and 2 cycles for the 16-bit-flag
// path (low byte + high byte). The caller supplies which flag (M or X) gates
// the width.
//
// wide_cond must be the TimingCondition that is TRUE when the register is
// 16-bit: kAccumulator16 for A, kIndex16 for X/Y.
constexpr CycleFragment LoadRegFromAddr(Reg reg, TimingCondition wide_cond) {
  const uint8_t pack_low_8 = micro_op_params::PackLoadReg(reg, ByteSel::kLow, /*update_nz=*/true);
  const uint8_t pack_low_16 =
      micro_op_params::PackLoadReg(reg, ByteSel::kLow, /*update_nz=*/false, /*post_inc_addr=*/true);
  const uint8_t pack_high_16 = micro_op_params::PackLoadReg(reg, ByteSel::kHigh, /*update_nz=*/true);
  return Fragment()
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kLoadReg, Not(Condition(wide_cond)),
                          "read (8-bit)", pack_low_8})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kLoadReg, Condition(wide_cond), "read low",
                          pack_low_16})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kLoadReg, Condition(wide_cond), "read high",
                          pack_high_16})
      .Build();
}

// Apply an ALU operation (ADC/SBC/AND/ORA/EOR/CMP) against the byte(s) at
// addr_, respecting the accumulator-width flag. One cycle for the 8-bit path
// and two cycles for the 16-bit path (low byte into addr_scratch_, high byte
// performs the 16-bit ALU using scratch low + fetch_data_ high).
//
// wide_cond must be kAccumulator16 for A-based ALU ops. This helper is not
// used for CPX/CPY (they use immediate-only in the current opcode set).
constexpr CycleFragment AluFromAddr(AluOp op, TimingCondition wide_cond) {
  return Fragment()
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kAlu8Imm, Not(Condition(wide_cond)),
                          "read + ALU (8-bit)", micro_op_params::PackAluOp(op)})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kStashIndirectLow, Condition(wide_cond),
                          "read low", 0})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kAlu16Imm, Condition(wide_cond),
                          "read high + ALU (16-bit)", micro_op_params::PackAluOp(op, /*low_from_scratch=*/true)})
      .Build();
}

// Store a register (A/X/Y) to addr_, respecting the width flag. Mirrors
// StoreAccumulator / StoreIndexX / StoreIndexY from cpu_opcodes.cpp but
// generalized over (WriteSrc, wide_cond). Uses the shared-params trick: the
// low-byte slot pairs kWriteRegByte with kModifyAddr(increment), so addr_
// advances into the high byte between slots.
constexpr CycleFragment StoreRegToAddr(WriteSrc src, TimingCondition wide_cond) {
  return Fragment()
      .Then(WriteRegByte(src, ByteSel::kLow, MicroInternalOp::kModifyAddr, Always(), "write low"))
      .Then(WriteRegByte(src, ByteSel::kHigh, MicroInternalOp::kNone, Condition(wide_cond), "write high"))
      .Build();
}

}  // namespace pupsnes::opcode_defs_internal
