#pragma once

// Reusable CycleFragment helpers for 65C816 addressing modes. Each helper
// returns a CycleFragment that encodes the cycles needed to compute an
// effective address into addr_, or to read/write a register through addr_,
// honoring width flags (M / X) and per-mode timing penalties (e.g. "+w" when
// the DP low byte is nonzero).
//
// Fragments here are composed with OpcodeSpecBuilder in cpu_opcodes.cpp the
// same way FetchAbsoluteAddr / StoreAccumulator are today — see
// cpu_opcodes.cpp for the original patterns.

#include "pupsnes/5a22/cpu_opcode_defs_internal.h"

namespace pupsnes::opcode_defs_internal {

// ---------------------------------------------------------------------------
// Address computation
// ---------------------------------------------------------------------------

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
      .Then(Internal(MicroInternalOp::kNone, Condition(TimingCondition::kDirectPageLowNonzero),
                     "DP-low-nonzero penalty"))
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
      .Then(Internal(MicroInternalOp::kNone, Condition(TimingCondition::kDirectPageLowNonzero),
                     "DP-low-nonzero penalty"))
      .Then(CycleSlotSpec{
          MicroBusAction::kNone,
          MicroInternalOp::kAddIndexToAddr,
          Always(),
          "add index",
          micro_op_params::PackAddIndex(index_reg),
      })
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
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kLoadReg,
                          Not(Condition(wide_cond)), "read (8-bit)", pack_low_8})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kLoadReg,
                          Condition(wide_cond), "read low", pack_low_16})
      .Then(CycleSlotSpec{MicroBusAction::kReadAddr, MicroInternalOp::kLoadReg,
                          Condition(wide_cond), "read high", pack_high_16})
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
