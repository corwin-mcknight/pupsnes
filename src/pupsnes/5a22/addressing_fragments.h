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
constexpr CycleSlotSpec FetchAddrByte(ByteSel byte_sel, bool from_dbr, TimingRuleExpr rule, std::string_view label) {
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
          micro_op_params::PackAddIndex(index_reg, /*bank_wrap=*/true, /*dp_wrap=*/true),
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
  // Pointer advance uses kStashDpIndirectLow so an emulation-mode pointer
  // crossing the DP-page boundary wraps back to DP base instead of advancing
  // into the next page (E=1 + DPL=$00 quirk per Bruce Clark §6.2).
  return Fragment()
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kStashDpIndirectLow, Always(),
                          "read pointer low", 0})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kFormAddrFromScratchDbr, Always(),
                          "read pointer high, assemble", 0})
      .Build();
}

// Direct indexed indirect X: add X to addr (bank-0 wrap), then read a 2-byte
// pointer at the indexed DP address and assemble into DBR:(high:low). Three
// additional cycles on top of FetchDirectPage. Used by (dp,X) addressing.
// Cycle formula 7-m+w (Bruce Clark §6.1.1.1) — X add bank-wraps in the DP
// arithmetic per 65C816 (DP + X stays in bank 0 even on overflow).
constexpr CycleFragment FetchDirectIndexedIndirectX() {
  return Fragment()
      .Then(CycleSlotSpec{
          MicroBusAction::kNone,
          MicroInternalOp::kAddIndexToAddr,
          Always(),
          "add X to DP addr",
          micro_op_params::PackAddIndex(Reg::kX, /*bank_wrap=*/true, /*dp_wrap=*/true),
      })
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kStashDpXIndirectLow, Always(),
                          "read pointer low", 0})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kFormAddrFromScratchDbr, Always(),
                          "read pointer high, assemble", 0})
      .Build();
}

// Direct indirect indexed Y: read a 2-byte pointer from bank 0 (DP-derived
// addr), assemble into DBR:(high:low), then add Y with 24-bit carry into the
// bank byte. Three additional cycles on top of FetchDirectPage. Used by
// (dp),Y addressing. Always pays the index-add cycle (matches the abs,X
// model — overcounts the no-page-cross case by one cycle, awaiting a future
// kIndexedPageCrossed condition).
constexpr CycleFragment FetchDirectIndirectIndexedY() {
  return Fragment()
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kStashDpIndirectLow, Always(),
                          "read pointer low", 0})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kFormAddrFromScratchDbr, Always(),
                          "read pointer high, assemble", 0})
      .Then(CycleSlotSpec{
          MicroBusAction::kNone,
          MicroInternalOp::kAddIndexToAddr,
          Always(),
          "add Y to addr",
          micro_op_params::PackAddIndex(Reg::kY, /*bank_wrap=*/false),
      })
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

// Direct indirect long indexed Y: read a 3-byte pointer from bank 0 (DP-derived
// addr), assemble bank:high:low, and add Y with 24-bit carry into the bank
// byte — all in three cycles. The Y add is folded into the bank-fetch cycle
// via PackFormAddrFromScratchBank(with_y_add=true), matching Bruce Clark's
// "7-m+w" formula exactly (no separate index-add cycle). Used by [dp],Y
// addressing.
constexpr CycleFragment FetchDirectIndirectLongIndexedY() {
  return Fragment()
      .Then(
          CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kStashIndirectLow, Always(), "read pointer low", 0})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kStashIndirectHigh, Always(), "read pointer high",
                          0})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kFormAddrFromScratchBank, Always(),
                          "read pointer bank, assemble + add Y",
                          micro_op_params::PackFormAddrFromScratchBank(/*with_y_add=*/true)})
      .Build();
}

// Stack-relative indirect indexed Y: composed after FetchStackRelative (which
// leaves addr_ = bank 0 : (SP + offset)). Reads a 2-byte pointer from bank 0,
// assembles into DBR:(high:low), then adds Y with 24-bit carry. Three
// additional cycles on top of FetchStackRelative. Used by (sr,S),Y addressing
// (Bruce Clark §5.21; cycle formula 8-m). Structurally identical to
// FetchDirectIndirectIndexedY — both read a 16-bit bank-0 pointer and add Y —
// but named for clarity at the call site.
constexpr CycleFragment FetchStackRelativeIndirectIndexedY() {
  return Fragment()
      .Then(
          CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kStashIndirectLow, Always(), "read pointer low", 0})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kFormAddrFromScratchDbr, Always(),
                          "read pointer high, assemble", 0})
      .Then(CycleSlotSpec{
          MicroBusAction::kNone,
          MicroInternalOp::kAddIndexToAddr,
          Always(),
          "add Y to addr",
          micro_op_params::PackAddIndex(Reg::kY, /*bank_wrap=*/false),
      })
      .Build();
}

// JMP (abs): fetch 16-bit operand forming a bank-0 pointer address, then
// read the 2-byte destination and jump within the current program bank.
// Cycle formula 5 (Bruce Clark §6.2.2.1 — 5C opcode is JMP long; 6C is JMP
// (abs)). Four remaining cycles on top of the opcode fetch:
//   1. fetch pointer low
//   2. fetch pointer high, force bank=0
//   3. read pointer low + stash
//   4. read pointer high + set PC = fetch:scratch_low (PBR unchanged).
// Pointer reads happen in bank 0 regardless of PBR (per 65C816 spec).
constexpr CycleFragment FetchJumpAbsoluteIndirect() {
  return Fragment()
      .Then(FetchAddrByte(ByteSel::kLow, false, Always(), "fetch pointer low"))
      .Then(CycleSlotSpec{
          MicroBusAction::kFetchPc,
          MicroInternalOp::kSetAddrByteFromFetch,
          Always(),
          "fetch pointer high, bank=0",
          micro_op_params::PackSetAddrByte(ByteSel::kHigh, BankSrc::kZero),
      })
      .Then(
          CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kStashIndirectLow, Always(), "read target low", 0})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kSetPcFromScratchAndFetch, Always(),
                          "read target high, set PC",
                          /*with_pbr=*/0})
      .Build();
}

// JMP [abs]: like JMP (abs) but with a 24-bit pointer. Cycle count 6
// (Bruce Clark §6.2.2.1, DC opcode). Five remaining cycles:
//   1. fetch pointer low
//   2. fetch pointer high, force bank=0
//   3. read pointer low + stash
//   4. read pointer high + stash
//   5. read pointer bank + set PC = scratch[15:0], PBR = fetch_data_.
// The final cycle uses kSetPcFromScratchAndFetch(with_pbr=true) to route the
// just-fetched bank byte into PBR.
constexpr CycleFragment FetchJumpAbsoluteIndirectLong() {
  return Fragment()
      .Then(FetchAddrByte(ByteSel::kLow, false, Always(), "fetch pointer low"))
      .Then(CycleSlotSpec{
          MicroBusAction::kFetchPc,
          MicroInternalOp::kSetAddrByteFromFetch,
          Always(),
          "fetch pointer high, bank=0",
          micro_op_params::PackSetAddrByte(ByteSel::kHigh, BankSrc::kZero),
      })
      .Then(
          CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kStashIndirectLow, Always(), "read target low", 0})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kStashIndirectHigh, Always(), "read target high",
                          0})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kSetPcFromScratchAndFetch, Always(),
                          "read target bank, set PC+PBR",
                          /*with_pbr=*/1})
      .Build();
}

// JMP/JSR (abs,X): fetch 16-bit operand, add X with bank-wrap in PBR,
// then read the 2-byte destination. Five extra cycles on top of the base
// opcode fetch (and push phase for JSR). Pointer reads happen in PBR
// (K:(HHLL+X)) per Bruce Clark §5.5.
// This helper covers the pointer-fetch sequence; callers prepend JSR's
// push phase as needed. Cycle count contribution:
//   1. fetch pointer low
//   2. fetch pointer high, bank=PBR
//   3. internal add X (bank-wrap within PBR)
//   4. read pointer low + stash
//   5. read pointer high + set PC = fetch:scratch_low (PBR unchanged).
constexpr CycleFragment FetchJumpAbsoluteIndexedIndirectX() {
  return Fragment()
      .Then(FetchAddrByte(ByteSel::kLow, false, Always(), "fetch pointer low"))
      .Then(CycleSlotSpec{
          MicroBusAction::kFetchPc,
          MicroInternalOp::kSetAddrByteFromFetch,
          Always(),
          "fetch pointer high, bank=PBR",
          micro_op_params::PackSetAddrByte(ByteSel::kHigh, BankSrc::kPbr),
      })
      .Then(CycleSlotSpec{
          MicroBusAction::kNone,
          MicroInternalOp::kAddIndexToAddr,
          Always(),
          "add X to pointer",
          micro_op_params::PackAddIndex(Reg::kX, /*bank_wrap=*/true),
      })
      .Then(
          CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kStashIndirectLow, Always(), "read target low", 0})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kSetPcFromScratchAndFetch, Always(),
                          "read target high, set PC",
                          /*with_pbr=*/0})
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
// The 16-bit advance uses kStashOperandLow (24-bit carry) so an absolute-mode
// read at $xxFFFF correctly pulls its high byte from $(xx+1):0000. Using
// kStashIndirectLow here would bank-wrap and read the wrong byte.
//
// wide_cond must be kAccumulator16 for A-based ALU ops. This helper is not
// used for CPX/CPY (they use immediate-only in the current opcode set).
constexpr CycleFragment AluFromAddr(AluOp op, TimingCondition wide_cond) {
  return Fragment()
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kAlu8Imm, Not(Condition(wide_cond)),
                          "read + ALU (8-bit)", micro_op_params::PackAluOp(op)})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kStashOperandLow, Condition(wide_cond),
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

// Read-modify-write through addr_. Covers ASL/LSR/ROL/ROR/INC/DEC on memory.
// Width follows the M flag and is dispatched dynamically by kRmwMem rather
// than encoded into separate slots; this keeps the total slot count to 5
// (3 unconditional + 2 gated on kAccumulator16) so the helper composes with
// FetchAbsoluteIndexed (3 slots) and FetchDirectPageIndexed (3 slots) without
// overflowing kMaxRemainingOps = 8.
//
// Cycle sequence, 8-bit (M=1): read → modify → write. 3 cycles.
// Cycle sequence, 16-bit (M=0): read-lo → read-hi → modify → write-hi →
// write-lo. 5 cycles.
//
// kRmwMem rolls addr_ back by one in the 8-bit path so the subsequent paired
// write cycle (kWriteRegByte + kModifyAddr(decrement)) lands at the original
// effective address. In 16-bit the stash/advance on the first read leaves
// addr_ pointing at the high byte, so the same paired write emits high first,
// decrements, and the conditional final slot writes the low byte from
// addr_scratch_.
//
// The read-advance uses kStashOperandLow (24-bit carry) so that a 16-bit
// RMW at $xxFFFF reads its high byte from $(xx+1):0000 — and so kRmwMem's
// 24-bit rollback and kModifyAddr's 24-bit decrement on the write path stay
// consistent with the read advance across the bank boundary.
constexpr CycleFragment ReadModifyWriteFromAddr(RmwOp op) {
  // Shared-params trick: kWriteRegByte reads bits [3:0]; kModifyAddr reads
  // bit 4 for decrement. Combine the two so a single params byte encodes both.
  constexpr auto kPackWriteDec = [](WriteSrc src) {
    return static_cast<uint8_t>(micro_op_params::PackWriteAddr(src, ByteSel::kLow) |
                                micro_op_params::PackModifyAddr(/*increment=*/false));
  };
  return Fragment()
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kStashOperandLow, Always(), "read byte / low", 0})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kNone, Condition(TimingCondition::kAccumulator16),
                          "read high (16-bit)", 0})
      .Then(CycleSlotSpec{MicroBusAction::kNone, MicroInternalOp::kRmwMem, Always(), "modify",
                          micro_op_params::PackRmw(op)})
      .Then(CycleSlotSpec{MicroBusAction::kWriteRegByte, MicroInternalOp::kModifyAddr, Always(),
                          "write byte / high, dec", kPackWriteDec(WriteSrc::kFetchData)})
      .Then(CycleSlotSpec{MicroBusAction::kWriteRegByte, MicroInternalOp::kNone,
                          Condition(TimingCondition::kAccumulator16), "write low (16-bit)",
                          micro_op_params::PackWriteAddr(WriteSrc::kScratchLow, ByteSel::kLow)})
      .Build();
}

}  // namespace pupsnes::opcode_defs_internal
