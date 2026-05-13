#pragma once

#include <cstdint>

#include "pupsnes/types.h"

namespace pupsnes {

// Bus actions a micro-op can perform in a single master clock cycle.
enum class MicroBusAction : uint8_t {
  kNone,             // Internal cycle — no bus transaction
  kFetchPc,          // Read byte from PBR:PC, increment PC
  kReadAddr,         // Read byte from effective address (addr_)
  kWriteRegByte,     // Write a byte to effective address (addr_); byte source
                     // selected by (WriteSrc, ByteSel) packed in
                     // CycleSlotSpec::params (see micro_op_params::PackWriteAddr).
                     // Dispatch lives in the kWriteRegByte case in PerformBusAction in cpu.cpp.
  kPushStack,        // Write a byte to stack ($00:SP); byte source selected by
                     // PushSrc packed in CycleSlotSpec::params[3:0] (see
                     // micro_op_params::PackPushStack). Dispatch lives in
                     // the kPushStack case in PerformBusAction in cpu.cpp.
  kPullStack,        // Read byte from stack ($00:SP) into fetch_data_
  kPreIncPullStack,  // Increment SP then read from stack into fetch_data_
};

// Internal register operations performed after the bus action completes.
enum class MicroInternalOp : uint8_t {
  kNone,
  kLoadReg,                   // reg_byte = fetch_data_; width/flag semantics per (reg, byte_sel,
                              // update_nz); params packs Reg (A/X/Y/Dp/Dbr/P) in
                              // bits [3:0], ByteSel (kLow/kHigh) in bits [5:4], and update_nz in
                              // bit 6 (see micro_op_params::PackLoadReg). Dispatch via
                              // the kLoadReg case in ExecuteInternalOp. For Reg::kP, emulation-mode forcing is handled
                              // by regs.P.FromByte(fetch, E); update_nz is ignored. PC/PBR have
                              // dedicated ops (kLoadPcLowFromFetch / kLoadPcHighFromFetch /
                              // kLoadPbrFromFetch) — they are not valid Reg targets.
  kLoadPcLowFromFetch,        // PC = (PC & 0xFF00) | fetch_data_. No params, no flag updates.
                              // Used by RTS/RTL/RTI to pull the return-address low byte into PC.
  kLoadPcHighFromFetch,       // PC = (PC & 0x00FF) | (fetch_data_ << 8). No params, no flag
                              // updates. Used by RTS/RTL/RTI to pull the return-address high byte.
  kLoadPbrFromFetch,          // PBR = fetch_data_. No params, no flag updates. Used by RTL and
                              // by RTI's native-mode PBR pull.
  kSetBranchTakenCond,        // branch_taken = <BranchCond(params[3:0])>; params packs the
                              // BranchCond (kAlways/kZ/kNotZ/kC/kNotC/kN/kNotN/kV/kNotV)
                              // in bits [3:0] (see micro_op_params::PackBranchCond)
  kBranchRelative,            // Apply signed branch offset to PC. Params bit 0 = wide: when 0,
                              // uses signed 8-bit displacement from fetch_data_; when 1, uses
                              // signed 16-bit displacement from addr_[15:0]. The 8-bit path also
                              // updates timing_context_.branch_page_crossed.
  kAddIndexToAddr,            // addr_ += index, width follows P.X (full 16-bit when X=0, low 8
                              // bits when X=1). Params[3:0] selects Reg::kX or Reg::kY;
                              // params[4] = bank_wrap: when 1 addr_ is masked to 16 bits
                              // (bank forced to 0, used by direct-page-indexed), when 0 the add
                              // is 24-bit and carry can propagate into the bank byte (used by
                              // absolute-indexed, where the effective address is DBR:(abs + idx)).
                              // See micro_op_params::PackAddIndex.
  kStashIndirectLow,          // addr_scratch_[7:0] = fetch_data_; addr_ low 16 bits += 1 with
                              // bank-wrap (bank byte untouched). First step of (dp) / [dp] /
                              // (dp),Y / [dp],Y and the JMP/JSR indirect family: captures the
                              // low byte of the indirect pointer while advancing addr_ to the
                              // next pointer byte. Bank-wrap matches the 65C816's pointer-fetch
                              // behavior — pointers in bank 0 / PBR stay in that bank when the
                              // low 16 bits overflow. For 16-bit operand reads through the
                              // effective address (ALU/RMW), use kStashOperandLow instead so
                              // the high byte address carries into (bank+1):0000.
  kStashIndirectHigh,         // addr_scratch_[15:8] = fetch_data_; addr_ low 16 bits += 1 with
                              // bank-wrap. Second step for [dp] / [dp],Y long-indirect and
                              // JMP [abs]: captures pointer high byte before the bank-byte read
                              // clobbers fetch_data_. Same bank-wrap reasoning as
                              // kStashIndirectLow.
  kStashOperandLow,           // addr_scratch_[7:0] = fetch_data_; addr_ += 1 with 24-bit carry
                              // (overflow at $xxFFFF -> $(xx+1):0000). Used as the low-byte
                              // step of a 16-bit data read through the effective address — the
                              // high byte lives at the next 24-bit address, so the bank must
                              // increment on rollover. Distinct from kStashIndirectLow, which
                              // bank-wraps for pointer fetches.
  kFormAddrFromScratchDbr,    // addr_ = DBR:(fetch_data_<<8 | addr_scratch_[7:0]). Completes
                              // assembly for (dp) / (dp,X) / (dp),Y: the just-fetched byte is
                              // the pointer high, scratch held the pointer low, and DBR supplies
                              // the bank for the operand access.
  kFormAddrFromScratchBank,   // addr_ = fetch_data_:(addr_scratch_[15:8]:addr_scratch_[7:0]).
                              // Completes assembly for [dp] / [dp],Y: scratch held low+high of
                              // the indirect and the just-fetched byte is the bank.
  kSetAddrFromSp,             // addr_ = bank 0, (SP + fetch_data_) & 0xFFFF. Used by stack-relative
                              // addressing (sr,S). Always bank-0.
  kSetAddrFromDp,             // addr_ = bank 0, (DP + fetch_data_) & 0xFFFF. Used by direct-page
                              // addressing to turn the just-fetched DP offset into the effective
                              // address. Always bank-0; never wraps the DP+offset inside a page.
                              // The "DP low-byte nonzero" +1 cycle penalty is handled by the
                              // instruction's timing rule (see kDirectPageLowNonzero alias).
  kSetAddrByteFromFetch,      // addr_[byte] = fetch_data_. Params bits [1:0] = ByteSel (kLow,
                              // kHigh, kBank), bit [2] = from_dbr. from_dbr is only meaningful
                              // when byte_sel == kHigh: in that combination addr_[15:8] is set
                              // from fetch and addr_[23:16] is set from DBR.
  kModifyAddr,                // addr_ += 1 (bit 0 set) or addr_ -= 1 (bit 0 clear). Wraps at 24
                              // bits. Only increment is currently used; decrement encoding is
                              // reserved.
  kModifySp,                  // SP += 1 or SP -= 1 (bit 0 = increment). Wraps in page 1 when E=1.
  kModifyPc,                  // PC += 1 or PC -= 1 (bit 0 = increment). 16-bit wrap.
  kIncDecReg,                 // reg += 1 or reg -= 1; width/flag semantics per reg; params
                              // packs decrement flag in bit 0 and Reg (A/X/Y) in bits [4:1]
                              // (see micro_op_params::PackIncDec)
  kSetFlag,                   // P.<flag> = value; params packs value in bit 0 and Flag
                              // (C/D/I/V only) in bits [4:1] (see micro_op_params::PackSetFlag)
  kMaskStatus,                // P mask update. Params bit 0: 1 = SEP (P |= fetch_data_), 0 = REP
                              // (P &= ~fetch_data_). Both paths preserve the E-mode forcing of
                              // M/X back to 1 via regs.P.FromByte + ApplyEmulationForcing.
  kExchangeCarryEmulation,    // swap C and E; on E=1 force M,X=1, XH/YH=0, SH=$01
  kSwapBA,                    // XBA: swap the high and low bytes of the 16-bit A register.
                              // N and Z are always set from the new low byte (i.e. the prior
                              // high byte) regardless of the M flag (Bruce Clark §6.10.3).
                              // No params.
  kAddPcToAddr,               // PER: addr_[15:0] = (PC + addr_[15:0]) & 0xFFFF. Used to
                              // compute the PC-relative effective address a PER pushes after
                              // the signed 16-bit displacement has been stashed into addr_[15:0]
                              // by two kSetAddrByteFromFetch cycles. Bank byte of addr_ is left
                              // unchanged; callers only consume addr_[15:0] via PushSrc::
                              // kAddrHigh/kAddrLow. No params.
  kSetInterruptVector,        // BRK/COP/HW interrupts: addr_ = <interrupt vector for (kind, E)>.
                              // Params bits [2:0] select InterruptKind (kBrk=0, kCop=1, kNmi=2,
                              // kIrq=3, kAbort=4). Native (E=0): BRK=$00FFE6, COP=$00FFE4,
                              // NMI=$00FFEA, IRQ=$00FFEE, ABORT=$00FFE8. Emulation (E=1):
                              // BRK=$00FFFE, COP=$00FFF4, NMI=$00FFFA, IRQ=$00FFFE (same slot
                              // as BRK — distinguishable only by the B flag pushed in P),
                              // ABORT=$00FFF8. See micro_op_params::PackSetInterruptVector
                              // and Bruce Clark §6.3.1 / §6.11 / WDC §9.
  kEnterInterruptHandler,     // BRK/COP/HW interrupt entry — consumes the 16-bit handler PC built
                              // from addr_scratch_[7:0] (low, stashed by a prior kStashIndirectLow)
                              // and fetch_data_ (high). Sets PC = fetch_data_:scratch_low, PBR = 0,
                              // I = 1, D = 0. No params.
  kHaltCpu,                   // STP / WAI: set halt_state_ to kStp or kWai. TickToTarget
                              // consumes master cycles without fetching or executing further
                              // instructions. STP stays halted until Reset(). WAI sleeps until
                              // any interrupt pin asserts (NMI/IRQ/ABORT — regardless of I flag
                              // for wake, still gated by I for delivery), then takes a 2-cycle
                              // internal wake latency before resuming. Params bit 0 = is_stp
                              // (1 = STP, 0 = WAI); see micro_op_params::PackHaltCpu.
  kMoveSetDbr,                // MVN/MVP: DBR = fetch_data_ (no flag changes). Emitted in the
                              // cycle that fetches the destination-bank operand byte.
  kMoveSetAddrFromSrc,        // MVN/MVP: addr_ = (fetch_data_ << 16) | X, where fetch_data_
                              // holds the just-fetched source-bank byte. Emitted in the cycle
                              // that fetches the source-bank operand.
  kMoveSetAddrFromDst,        // MVN/MVP: addr_ = (DBR << 16) | Y. Emitted during the read cycle
                              // (paired with kReadAddr) so the next cycle's write lands at
                              // dest_bank:Y.
  kMoveAdjust,                // MVN/MVP post-move register update. A -= 1 (always 16-bit); when
                              // the X flag is 0, X and Y adjust as 16-bit; when X=1, only the
                              // low byte of X and Y is updated. Params bit 0 = decrement (1 =
                              // MVP: dec X/Y, 0 = MVN: inc X/Y).
  kMoveLoopCheck,             // MVN/MVP: if A != $FFFF then PC -= 3 (re-execute the 3-byte
                              // instruction for the next byte). When A == $FFFF the move is
                              // complete and PC (already past the instruction) is unchanged.
  kTransferReg,               // dst = src; width/flag semantics per (src,dst) pair; params
                              // packs src in [3:0] and dst in [7:4] (see micro_op_params::PackTransfer)
  kLoadAddrByteAndSetPc,      // Fused "set addr byte from fetch + set PC from addr". Params
                              // bits [1:0] = ByteSel (kHigh for JMP abs; kBank for JML),
                              // bit [2] = with_pbr (1 for JML: also set PBR from addr).
  kAlu8Imm,                   // 8-bit ALU immediate: apply AluOp to fetch_data_ against
                              // A/X/Y (or just update Z for BIT imm). Params pack AluOp in
                              // bits [3:0] (see micro_op_params::PackAluOp). Dispatch lives
                              // in the kAlu8Imm case in ExecuteInternalOp in cpu.cpp.
  kShiftRotateA,              // ASL/LSR/ROL/ROR on A. Width follows M flag.
                              // Params[1:0] = ShiftOp (kAsl/kLsr/kRol/kRor).
                              // Dispatch lives in the kShiftRotateA case in
                              // ExecuteInternalOp in cpu.cpp.
  kAlu16Imm,                  // 16-bit ALU immediate: apply AluOp to the 16-bit operand
                              // formed from addr_[7:0] (low, stashed by a prior
                              // kSetAddrByteFromFetch(kLow)) and fetch_data_ (high). Params
                              // pack AluOp in bits [3:0]. Dispatch lives in the kAlu16Imm case in ExecuteInternalOp
                              // in cpu.cpp.
  kSetPcFromScratchAndFetch,  // PC/PBR load from indirect-pointer assembly. Params bit 0 =
                              // with_pbr: when 0 (JMP (abs) / JMP (abs,X)) PC =
                              // fetch_data_:addr_scratch_[7:0] and PBR is unchanged; when 1
                              // (JMP [abs]) PC = addr_scratch_[15:0] and PBR = fetch_data_.
                              // The prior cycle's kStashIndirectLow/kStashIndirectHigh
                              // populated addr_scratch_; this cycle's kReadAddr supplies the
                              // last pointer byte in fetch_data_.
  kRmwMem,                    // Memory read-modify-write. Params bits [2:0] = RmwOp
                              // (kAsl/kLsr/kRol/kRor/kInc/kDec). Width follows the M flag
                              // at runtime:
                              //   M=1 (8-bit): fetch_data_ holds the byte just read; the op
                              //     modifies fetch_data_ in place, updates N/Z/C flags, AND
                              //     decrements addr_ by 1 so the subsequent paired-write
                              //     cycle (kWriteRegByte + kModifyAddr(decrement)) lands at
                              //     the original effective address.
                              //   M=0 (16-bit): a prior kStashOperandLow stashed the low
                              //     byte into addr_scratch_[7:0] and advanced addr_ past the
                              //     high byte (with 24-bit carry, so absolute-mode reads at
                              //     $xxFFFF correctly land in $(xx+1):0000). fetch_data_ holds
                              //     the high byte just read.
                              //     The op combines the 16-bit operand, modifies it, and
                              //     writes new_high back to fetch_data_ and new_low back to
                              //     addr_scratch_[7:0]. addr_ is left at the high-byte
                              //     address (original+1) so the write phase emits
                              //     high-first-then-low in standard 65C816 order.
};

// Maximum micro-ops remaining after the opcode fetch. LDA [dp] with 16-bit
// accumulator needs 8 slots (fetch DP offset + DL-penalty + 3 indirect reads
// + 3-slot LoadRegFromAddr), which bounds the limit. Used by MicroOpRecord
// debugger observers; the matching InstructionEntry::ops size lives in
// cpu_internal.h.
inline constexpr uint8_t kMaxRemainingOps = 8;

enum class MicroOpStatus : uint8_t {
  kPending = 0,
  kExecuted = 1,
  kSkipped = 2,
};

struct MicroOpRecord {
  uint8_t index = 0;  // 0 = opcode fetch, 1..N = remaining ops (1-based)
  MicroBusAction bus_action = MicroBusAction::kNone;
  MicroInternalOp internal_op = MicroInternalOp::kNone;
  uint8_t params = 0;
  MicroOpStatus status = MicroOpStatus::kPending;
  uint8_t fetch_data = 0;
  uint32_t addr = 0;
  bool has_bus = false;
  uint32_t bus_addr = 0;
  uint8_t bus_value = 0;
};

class MicroOpRecorder {
 public:
  virtual ~MicroOpRecorder() = default;
  virtual void OnInstructionBegin(uint8_t opcode, SnesAddrT pc) = 0;
  virtual void OnMicroOp(const MicroOpRecord& rec) = 0;
  virtual void OnInstructionEnd(uint64_t retired_seq) = 0;
};

}  // namespace pupsnes
