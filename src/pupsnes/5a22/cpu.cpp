#include "pupsnes/hw/5a22/cpu.h"

#include <cassert>
#include <cstdint>
#include <optional>
#include <stdexcept>

#include "pupsnes/5a22/cpu_internal.h"
#include "pupsnes/5a22/cpu_opcode_defs_internal.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"

namespace pupsnes {

// ---------------------------------------------------------------------------
// CpuFlags
// ---------------------------------------------------------------------------

uint8_t CpuFlags::ToByte() const {
  uint8_t p = 0;
  if (N) p |= 0x80U;
  if (V) p |= 0x40U;
  if (M) p |= 0x20U;
  if (X) p |= 0x10U;
  if (D) p |= 0x08U;
  if (I) p |= 0x04U;
  if (Z) p |= 0x02U;
  if (C) p |= 0x01U;
  return p;
}

void CpuFlags::FromByte(uint8_t p, bool emulation_mode) {
  N = (p & 0x80U) != 0U;
  V = (p & 0x40U) != 0U;
  // In emulation mode M and X are forced to 1 and cannot be changed via P.
  if (!emulation_mode) {
    M = (p & 0x20U) != 0U;
    X = (p & 0x10U) != 0U;
  }
  D = (p & 0x08U) != 0U;
  I = (p & 0x04U) != 0U;
  Z = (p & 0x02U) != 0U;
  C = (p & 0x01U) != 0U;
}

// ---------------------------------------------------------------------------
// Micro-op internal helpers
//
// File-local helpers for the MicroInternalOp switch in ExecuteInternalOp.
// They operate on CpuRegs (+ small pieces of CPU internal state passed by
// reference) so the bodies live out of the header entirely — cpu.h only needs
// the ExecuteInternalOp declaration. Hot-path inlining is preserved because
// ExecuteInternalOp and all its callees share this TU.
// ---------------------------------------------------------------------------

namespace {

[[gnu::always_inline]] inline bool IsAccumulator16Bit(const CpuRegs& r) { return !r.P.E && !r.P.M; }
[[gnu::always_inline]] inline bool IsIndex16Bit(const CpuRegs& r) { return !r.P.E && !r.P.X; }
[[gnu::always_inline]] inline SnesAddrT StackAddr(const CpuRegs& r) { return static_cast<SnesAddrT>(r.SP); }
[[gnu::always_inline]] inline SnesAddrT PcAddr(const CpuRegs& r) {
  return (static_cast<uint32_t>(r.PBR) << 16U) | static_cast<uint32_t>(r.PC);
}

// Width-aware N/Z update: `wide` selects between 16-bit and 8-bit-low result.
[[gnu::always_inline]] inline void SetNzFromWidth(CpuRegs& regs, uint16_t value, bool wide) {
  if (wide) {
    regs.P.Z = (value == 0U);
    regs.P.N = (value & 0x8000U) != 0U;
  } else {
    const uint8_t lo = static_cast<uint8_t>(value);
    regs.P.Z = (lo == 0U);
    regs.P.N = (lo & 0x80U) != 0U;
  }
}

// When e=1 the m and x flags are forced to 1, XH/YH forced to $00, and the
// stack is forced onto page 1 (SH = $01). Called after any op that can
// change P or e.
[[gnu::always_inline]] inline void ApplyEmulationForcing(CpuRegs& regs) {
  if (regs.P.E) {
    regs.P.M = true;
    regs.P.X = true;
    regs.X &= 0x00FFU;
    regs.Y &= 0x00FFU;
    regs.SP = static_cast<uint16_t>(0x0100U | (regs.SP & 0x00FFU));
  }
}

// Mutable reference to the 16-bit GPR slot selected by `reg`. Used by
// kLoadReg / kIncDecReg / kTransferReg to share the read-modify-write pattern
// across A, X, Y, SP, and DP. Not valid for Reg::kDbr/kPbr/kPcl/kPch/kP —
// those are byte-sized or synthesized and handled inline at the call site.
[[gnu::always_inline]] inline uint16_t& RegRef(CpuRegs& r, Reg reg) {
  switch (reg) {
    case Reg::kA: return r.A;
    case Reg::kX: return r.X;
    case Reg::kY: return r.Y;
    case Reg::kSp: return r.SP;
    case Reg::kDp: return r.DP;
    default: break;
  }
  __builtin_unreachable();
}

}  // namespace

// ---------------------------------------------------------------------------
// CPU
// ---------------------------------------------------------------------------

CPU::CPU(SNES* snes) : Device(snes) {}

void CPU::Reset() {
  regs_ = Regs();
  regs_.SP = 0x01FFU;
  regs_.P = CpuFlags{};
  regs_.P.E = true;
  regs_.P.M = true;
  regs_.P.X = true;
  regs_.P.I = true;

  micro_op_index_ = 0;
  fetch_data_ = 0;
  addr_ = 0;
  timing_context_ = TimingContext{};
  fault_.reset();
  last_debugger_stop_.reset();
  retired_instruction_count_ = 0;
  current_instr_ = nullptr;
  needs_drain_ = false;
  system_bus_raw_ = (snes_ != nullptr) ? snes_->system_bus.get() : nullptr;
  local_time_ = (snes_ != nullptr) ? snes_->GetMasterTime() : 0;

  // First DRAM refresh fires kDramRefreshStartCycle master cycles after reset.
  next_refresh_time_ = local_time_ + kDramRefreshStartCycle;
  refresh_cycles_remaining_ = 0;
  retired_refresh_windows_ = 0;
  retired_refresh_cycles_ = 0;

  const uint8_t vector_lo = ReadResetVectorByte(0x00FFFCU);
  const uint8_t vector_hi = ReadResetVectorByte(0x00FFFDU);

  regs_.PBR = 0;
  regs_.PC = static_cast<uint16_t>(static_cast<uint16_t>(vector_hi) << 8U) | vector_lo;
}

void CPU::FinishInstruction() {
  if (micro_op_recorder_ != nullptr && current_instr_ != nullptr) {
    micro_op_recorder_->OnInstructionEnd(retired_instruction_count_ + 1);
  }
  // Trace-push is success-only: callers that retire via a fault (RecordFault)
  // skip the push so the faulted instruction is not logged as executed.
  if (current_instr_ != nullptr && debugger_contract_.trace_sink != nullptr) {
    debugger_contract_.trace_sink->Record(pending_trace_);
  }
  retired_instruction_count_++;
  current_instr_ = nullptr;
  needs_drain_ = false;
  micro_op_index_ = 0;
  timing_context_ = TimingContext{};
}

void CPU::DrainSkippedMicroOpsSlow() {
  while (current_instr_ != nullptr) {
    const InstructionEntry* instr = current_instr_;
    const MicroOp* const ops = instr->ops.data();
    const uint32_t* const rules = instr->rules.data();
    const uint8_t remaining = instr->remaining_op_count;

    const uint8_t op_idx = static_cast<uint8_t>(micro_op_index_ - 1U);
    assert(op_idx < remaining);

    const MicroOp& mop = ops[op_idx];
    assert(mop.rule_index < instr->rule_count);
    if (EvaluateTimingRule(rules[mop.rule_index])) {
      return;
    }

    if (micro_op_recorder_ != nullptr) {
      MicroOpRecord rec;
      rec.index = micro_op_index_;
      rec.bus_action = mop.bus_action;
      rec.internal_op = mop.internal_op;
      rec.params = mop.params;
      rec.status = MicroOpStatus::kSkipped;
      rec.fetch_data = fetch_data_;
      rec.addr = addr_;
      micro_op_recorder_->OnMicroOp(rec);
    }

    ++micro_op_index_;
    if (micro_op_index_ - 1U >= remaining) {
      FinishInstruction();
    }
  }
}

void CPU::RecordFault(Fault::Type type, uint8_t opcode, SnesAddrT opcode_address) {
  fault_ = Fault{type, opcode, opcode_address, regs_};
  FinishInstruction();
}

BusFollowResult CPU::PlanAndFollow(SnesAddrT addr, BusAccessType type, uint8_t data, TimeMasterDeltaT cycle_time) {
  auto plan = snes_->system_bus->Plan(addr, type, data);
  return snes_->system_bus->Follow(plan, local_time_ + cycle_time, device_id_);
}

uint8_t CPU::ReadResetVectorByte(SnesAddrT addr) {
  if (snes_ == nullptr || snes_->system_bus == nullptr) {
    return 0xFFU;
  }

  auto result = PlanAndFollow(addr, BusAccessType::kRead, 0, 0);
  if (result.outcome == BusPlanOutcome::kScheduledComplete) {
    throw std::logic_error("CPU reset vector fetch cannot block on asynchronous bus access");
  }
  return result.data;
}

TickResult CPU::BusReadSlow(SnesAddrT addr, TimeMasterDeltaT cycle_time) {
  auto result = PlanAndFollow(addr, BusAccessType::kRead, 0, cycle_time);
  if (result.outcome == BusPlanOutcome::kScheduledComplete) {
    return TickResult{cycle_time, TickStopReason::kBlockedOnToken, result.token};
  }
  fetch_data_ = result.data;
  return TickResult{0, TickStopReason::kContinue};
}

TickResult CPU::BusWriteSlow(SnesAddrT addr, uint8_t data, TimeMasterDeltaT cycle_time) {
  auto result = PlanAndFollow(addr, BusAccessType::kWrite, data, cycle_time);
  if (result.outcome == BusPlanOutcome::kScheduledComplete) {
    return TickResult{cycle_time, TickStopReason::kBlockedOnToken, result.token};
  }
  return TickResult{0, TickStopReason::kContinue};
}

// BusRead / BusWrite / EvaluateTimingRule are small hot-path member methods
// whose only callers live in this TU. Forcing inlining here recovers the
// inlining the header-inline definitions used to provide — same-TU visibility
// lets the compiler honor the attribute without ODR concerns.
[[gnu::always_inline]] inline TickResult CPU::BusRead(SnesAddrT addr, TimeMasterDeltaT cycle_time) {
  uint8_t data;
  if (system_bus_raw_ != nullptr && system_bus_raw_->TryFastRead(addr, data)) {
    fetch_data_ = data;
    return TickResult{0, TickStopReason::kContinue};
  }
  return BusReadSlow(addr, cycle_time);
}

[[gnu::always_inline]] inline TickResult CPU::BusWrite(SnesAddrT addr, uint8_t data, TimeMasterDeltaT cycle_time) {
  if (system_bus_raw_ != nullptr && system_bus_raw_->TryFastWrite(addr, data)) {
    return TickResult{0, TickStopReason::kContinue};
  }
  return BusWriteSlow(addr, data, cycle_time);
}

[[gnu::always_inline]] inline bool CPU::EvaluateTimingRule(uint32_t truth_table) const {
  const uint32_t bits =
      (static_cast<uint32_t>(timing_context_.branch_taken) << static_cast<uint8_t>(TimingCondition::kBranchTaken)) |
      (static_cast<uint32_t>(IsAccumulator16Bit(regs_)) << static_cast<uint8_t>(TimingCondition::kAccumulator16)) |
      (static_cast<uint32_t>(IsIndex16Bit(regs_)) << static_cast<uint8_t>(TimingCondition::kIndex16)) |
      (static_cast<uint32_t>(regs_.P.E) << static_cast<uint8_t>(TimingCondition::kEmulationMode)) |
      (static_cast<uint32_t>(timing_context_.branch_page_crossed)
       << static_cast<uint8_t>(TimingCondition::kBranchPageCrossed));
  return ((truth_table >> bits) & 1U) != 0U;
}

void CPU::ExecuteInternalOp(MicroInternalOp op, [[maybe_unused]] uint8_t params) {
  namespace mp = opcode_defs_internal::micro_op_params;
  switch (op) {
    case MicroInternalOp::kNone:
      return;

    case MicroInternalOp::kLoadReg: {
      const Reg reg = mp::UnpackLoadRegReg(params);
      const ByteSel sel = mp::UnpackLoadRegByteSel(params);
      const bool nz = mp::UnpackLoadRegNz(params);
      const uint8_t fetch = fetch_data_;
      switch (reg) {
        case Reg::kA:
        case Reg::kX:
        case Reg::kY: {
          uint16_t& r = RegRef(regs_, reg);
          if (sel == ByteSel::kLow) {
            r = static_cast<uint16_t>((r & 0xFF00U) | fetch);
            if (nz) {
              regs_.P.Z = (static_cast<uint8_t>(r) == 0U);
              regs_.P.N = (r & 0x0080U) != 0U;
            }
          } else {  // kHigh — always updates NZ for A/X/Y.
            const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(fetch) << 8U);
            r = static_cast<uint16_t>((r & 0x00FFU) | high);
            regs_.P.Z = (r == 0U);
            regs_.P.N = (r & 0x8000U) != 0U;
          }
          return;
        }
        case Reg::kDp:
          // DP low never updates NZ; DP high always does. nz param is ignored.
          if (sel == ByteSel::kLow) {
            regs_.DP = static_cast<uint16_t>((regs_.DP & 0xFF00U) | fetch);
          } else {
            const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(fetch) << 8U);
            regs_.DP = static_cast<uint16_t>((regs_.DP & 0x00FFU) | high);
            regs_.P.Z = (regs_.DP == 0U);
            regs_.P.N = (regs_.DP & 0x8000U) != 0U;
          }
          return;
        case Reg::kDbr:
          regs_.DBR = fetch;
          regs_.P.Z = (regs_.DBR == 0U);
          regs_.P.N = (regs_.DBR & 0x80U) != 0U;
          return;
        case Reg::kPcl:
          regs_.PC = static_cast<uint16_t>((regs_.PC & 0xFF00U) | fetch);
          return;
        case Reg::kPch: {
          const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(fetch) << 8U);
          regs_.PC = static_cast<uint16_t>((regs_.PC & 0x00FFU) | high);
          return;
        }
        case Reg::kPbr:
          regs_.PBR = fetch;
          return;
        case Reg::kP:
          // P.FromByte handles emulation-mode forcing of M/X back to 1. nz is ignored.
          regs_.P.FromByte(fetch, regs_.P.E);
          return;
        default:
          break;
      }
      __builtin_unreachable();
    }

    case MicroInternalOp::kSetBranchTakenCond: {
      bool taken = false;
      switch (mp::UnpackBranchCond(params)) {
        case BranchCond::kAlways: taken = true; break;
        case BranchCond::kZ:      taken = regs_.P.Z; break;
        case BranchCond::kNotZ:   taken = !regs_.P.Z; break;
        case BranchCond::kC:      taken = regs_.P.C; break;
        case BranchCond::kNotC:   taken = !regs_.P.C; break;
        case BranchCond::kN:      taken = regs_.P.N; break;
        case BranchCond::kNotN:   taken = !regs_.P.N; break;
        case BranchCond::kV:      taken = regs_.P.V; break;
        case BranchCond::kNotV:   taken = !regs_.P.V; break;
      }
      timing_context_.branch_taken = taken;
      return;
    }

    case MicroInternalOp::kBranchRelative:
      if (mp::UnpackBranchRelativeWide(params)) {
        // BRL: 16-bit displacement stashed in addr_[15:0].
        const int16_t displacement = static_cast<int16_t>(static_cast<uint16_t>(addr_ & 0xFFFFU));
        regs_.PC = static_cast<uint16_t>(regs_.PC + displacement);
      } else {
        const int8_t displacement = static_cast<int8_t>(fetch_data_);
        const uint16_t old_pc = regs_.PC;
        regs_.PC = static_cast<uint16_t>(regs_.PC + displacement);
        timing_context_.branch_page_crossed = ((old_pc ^ regs_.PC) & 0xFF00U) != 0U;
      }
      return;

    case MicroInternalOp::kSetAddrByteFromFetch: {
      const ByteSel sel = mp::UnpackSetAddrByteSel(params);
      const unsigned shift = (sel == ByteSel::kLow) ? 0U : (sel == ByteSel::kHigh) ? 8U : 16U;
      const uint32_t mask = ~(uint32_t{0xFFU} << shift) & 0xFFFFFFU;
      addr_ = (addr_ & mask) | (static_cast<uint32_t>(fetch_data_) << shift);
      // from_dbr is only meaningful with kHigh: also set bank byte from DBR.
      if (sel == ByteSel::kHigh && mp::UnpackSetAddrFromDbr(params)) {
        addr_ = (addr_ & 0x00FFFFU) | (static_cast<uint32_t>(regs_.DBR) << 16U);
      }
      return;
    }

    case MicroInternalOp::kModifyAddr: {
      const uint32_t delta = mp::UnpackModifyAddrIncrement(params) ? 1U : UINT32_C(0xFFFFFF);
      addr_ = (addr_ + delta) & 0xFFFFFFU;
      return;
    }

    case MicroInternalOp::kModifySp: {
      const bool inc = mp::UnpackModifySpIncrement(params);
      if (regs_.P.E) {
        const uint8_t sp_lo = static_cast<uint8_t>(static_cast<uint8_t>(regs_.SP) + (inc ? 1U : 0xFFU));
        regs_.SP = static_cast<uint16_t>(0x0100U | sp_lo);
      } else {
        regs_.SP = static_cast<uint16_t>(regs_.SP + (inc ? 1U : 0xFFFFU));
      }
      return;
    }

    case MicroInternalOp::kModifyPc:
      regs_.PC = static_cast<uint16_t>(regs_.PC + (mp::UnpackModifyPcIncrement(params) ? 1U : 0xFFFFU));
      return;

    case MicroInternalOp::kIncDecReg: {
      const Reg reg = mp::UnpackIncDecReg(params);
      const bool dec = mp::UnpackIncDecDecrement(params);
      const bool wide = (reg == Reg::kA) ? IsAccumulator16Bit(regs_) : IsIndex16Bit(regs_);
      uint16_t& r = RegRef(regs_, reg);
      if (wide) {
        r = static_cast<uint16_t>(r + (dec ? 0xFFFFU : 0x0001U));
        regs_.P.Z = (r == 0U);
        regs_.P.N = (r & 0x8000U) != 0U;
      } else {
        const uint8_t lo = static_cast<uint8_t>(static_cast<uint8_t>(r) + (dec ? 0xFFU : 0x01U));
        r = static_cast<uint16_t>((r & 0xFF00U) | lo);
        regs_.P.Z = (lo == 0U);
        regs_.P.N = (lo & 0x80U) != 0U;
      }
      return;
    }

    case MicroInternalOp::kSetFlag: {
      const bool value = mp::UnpackSetFlagValue(params);
      switch (mp::UnpackSetFlagFlag(params)) {
        case Flag::kC: regs_.P.C = value; return;
        case Flag::kD: regs_.P.D = value; return;
        case Flag::kI: regs_.P.I = value; return;
        case Flag::kV: regs_.P.V = value; return;
        default: break;
      }
      __builtin_unreachable();
    }

    case MicroInternalOp::kMaskStatus: {
      const uint8_t p = regs_.P.ToByte();
      const uint8_t next = mp::UnpackMaskStatusOr(params)
                               ? static_cast<uint8_t>(p | fetch_data_)
                               : static_cast<uint8_t>(p & ~fetch_data_);
      regs_.P.FromByte(next, regs_.P.E);
      ApplyEmulationForcing(regs_);
      return;
    }

    case MicroInternalOp::kExchangeCarryEmulation: {
      const bool new_e = regs_.P.C;
      const bool new_c = regs_.P.E;
      regs_.P.E = new_e;
      regs_.P.C = new_c;
      ApplyEmulationForcing(regs_);
      return;
    }

    case MicroInternalOp::kTransferReg: {
      const Reg src = mp::UnpackTransferSrc(params);
      const Reg dst = mp::UnpackTransferDst(params);
      // Dst=SP: full 16-bit write, no flags, E-mode forces SH=$01.
      if (dst == Reg::kSp) {
        regs_.SP = RegRef(regs_, src);
        if (regs_.P.E) {
          regs_.SP = static_cast<uint16_t>(0x0100U | (regs_.SP & 0x00FFU));
        }
        return;
      }
      // Width rule:
      //   dst=DP                                 → always 16 (TCD).
      //   dst=A and src∈{DP,SP}                  → always 16 (TDC/TSC).
      //   dst=A and src∈{X,Y}                    → accumulator-sized (TXA/TYA).
      //   dst∈{X,Y}                              → index-sized (TAX/TAY/TXY/TYX/TSX).
      const bool wide =
          (dst == Reg::kDp) ||
          (dst == Reg::kA ? (src == Reg::kDp || src == Reg::kSp || IsAccumulator16Bit(regs_))
                          : IsIndex16Bit(regs_));
      uint16_t& d = RegRef(regs_, dst);
      const uint16_t s = RegRef(regs_, src);
      d = wide ? s : static_cast<uint16_t>((d & 0xFF00U) | (s & 0x00FFU));
      SetNzFromWidth(regs_, d, wide);
      return;
    }

    case MicroInternalOp::kSetPcFromAddr:
      regs_.PC = static_cast<uint16_t>(addr_ & 0xFFFFU);
      if (mp::UnpackSetPcWithPbr(params)) {
        regs_.PBR = static_cast<uint8_t>((addr_ >> 16U) & 0xFFU);
      }
      return;

    case MicroInternalOp::kLoadAddrByteAndSetPc: {
      const ByteSel sel = mp::UnpackLoadAddrByteAndSetPcSel(params);
      const unsigned shift = (sel == ByteSel::kLow) ? 0U : (sel == ByteSel::kHigh) ? 8U : 16U;
      const uint32_t mask = ~(uint32_t{0xFFU} << shift) & 0xFFFFFFU;
      addr_ = (addr_ & mask) | (static_cast<uint32_t>(fetch_data_) << shift);
      regs_.PC = static_cast<uint16_t>(addr_ & 0xFFFFU);
      if (mp::UnpackLoadAddrByteAndSetPcWithPbr(params)) {
        regs_.PBR = static_cast<uint8_t>((addr_ >> 16U) & 0xFFU);
      }
      return;
    }

    case MicroInternalOp::kAlu8Imm:
    case MicroInternalOp::kAlu16Imm: {
      // Width + sign/carry bit positions selected by the op variant. The
      // 8-bit path consumes fetch_data_ only; the 16-bit path also consumes
      // addr_[7:0] (low, stashed by a prior kSetAddrByteFromFetch(kLow)).
      const bool wide = (op == MicroInternalOp::kAlu16Imm);
      const uint16_t fetch_high = static_cast<uint16_t>(static_cast<uint16_t>(fetch_data_) << 8U);
      const uint16_t operand = wide
          ? static_cast<uint16_t>((addr_ & 0xFFU) | fetch_high)
          : static_cast<uint16_t>(fetch_data_);
      const uint16_t mask = wide ? 0xFFFFU : 0x00FFU;
      const uint16_t sign = wide ? 0x8000U : 0x0080U;
      const uint32_t carry = wide ? 0x10000U : 0x0100U;
      const AluOp alu = mp::UnpackAluOp(params);

      auto store_a = [&](uint16_t result) {
        regs_.A = wide ? result : static_cast<uint16_t>((regs_.A & 0xFF00U) | (result & 0xFFU));
      };

      switch (alu) {
        case AluOp::kAdc:
        case AluOp::kSbc: {
          // BCD/decimal mode is not yet implemented; D=1 falls back to binary.
          const uint16_t rhs = (alu == AluOp::kSbc) ? static_cast<uint16_t>(~operand & mask) : operand;
          const uint32_t a_val = regs_.A & mask;
          const uint32_t sum = a_val + rhs + (regs_.P.C ? 1U : 0U);
          const uint16_t result = static_cast<uint16_t>(sum & mask);
          regs_.P.V = ((~(a_val ^ rhs) & (a_val ^ result)) & sign) != 0U;
          regs_.P.C = (sum & carry) != 0U;
          regs_.P.Z = (result == 0U);
          regs_.P.N = (result & sign) != 0U;
          store_a(result);
          return;
        }
        case AluOp::kAnd:
        case AluOp::kOra:
        case AluOp::kEor: {
          const uint16_t a_val = regs_.A & mask;
          const uint16_t result = (alu == AluOp::kAnd)   ? static_cast<uint16_t>(a_val & operand)
                                  : (alu == AluOp::kOra) ? static_cast<uint16_t>(a_val | operand)
                                                         : static_cast<uint16_t>(a_val ^ operand);
          regs_.P.Z = (result == 0U);
          regs_.P.N = (result & sign) != 0U;
          store_a(result);
          return;
        }
        case AluOp::kCmp:
        case AluOp::kCpx:
        case AluOp::kCpy: {
          const uint16_t reg_val =
              (alu == AluOp::kCmp)   ? static_cast<uint16_t>(regs_.A & mask)
              : (alu == AluOp::kCpx) ? static_cast<uint16_t>(regs_.X & mask)
                                     : static_cast<uint16_t>(regs_.Y & mask);
          const uint32_t diff = static_cast<uint32_t>(reg_val) - static_cast<uint32_t>(operand);
          regs_.P.C = reg_val >= operand;
          regs_.P.Z = ((diff & mask) == 0U);
          regs_.P.N = (diff & sign) != 0U;
          return;
        }
        case AluOp::kBit: {
          // Immediate BIT only updates Z (not N/V). See docs/plans/6502opcodes.md.
          const uint16_t result = static_cast<uint16_t>((regs_.A & mask) & operand);
          regs_.P.Z = (result == 0U);
          return;
        }
      }
      __builtin_unreachable();
    }
  }
}

CPU::StepResult CPU::FetchOpcode(TimeMasterDeltaT cycle_time) {
  const SnesAddrT opcode_address = PcAddr(regs_);

  if (const auto* bp = debugger_contract_.breakpoints;
      bp != nullptr && bp->AnyEnabled() && bp->IsEnabled(opcode_address)) {
    if (debugger_contract_.suppressed_breakpoint_pc == opcode_address) {
      debugger_contract_.suppressed_breakpoint_pc.reset();
    } else {
      last_debugger_stop_ = TickStopReason::kDebuggerBreakpoint;
      return StepResult{false, TickResult{cycle_time, TickStopReason::kDebuggerBreakpoint}};
    }
  }

  pending_trace_ = TraceEntry{local_time_ + cycle_time, opcode_address, regs_};

  TickResult blocked = BusRead(opcode_address, cycle_time);
  if (blocked.reason != TickStopReason::kContinue) {
    return StepResult{false, blocked};
  }
  regs_.PC = static_cast<uint16_t>(regs_.PC + 1U);
  timing_context_ = TimingContext{};

  const InstructionEntry& entry = opcode_defs_internal::kOpcodeArtifacts.execution_table[fetch_data_];
  if (entry.disposition == InstructionDisposition::kFaultUnimplemented) {
    RecordFault(Fault::Type::kUnimplementedOpcode, fetch_data_, opcode_address);
    return StepResult{true, TickResult{static_cast<TimeMasterDeltaT>(cycle_time + 1U), TickStopReason::kFaulted}};
  }

  current_instr_ = &entry;
  needs_drain_ = entry.rule_count > 1;
  micro_op_index_ = 1;
  if (micro_op_recorder_ != nullptr) {
    micro_op_recorder_->OnInstructionBegin(fetch_data_, opcode_address);
    MicroOpRecord rec;
    rec.index = 0;
    rec.bus_action = MicroBusAction::kFetchPc;
    rec.internal_op = MicroInternalOp::kNone;
    rec.status = MicroOpStatus::kExecuted;
    rec.fetch_data = fetch_data_;
    rec.addr = addr_;
    rec.has_bus = true;
    rec.bus_addr = opcode_address;
    rec.bus_value = fetch_data_;
    micro_op_recorder_->OnMicroOp(rec);
  }
  DrainSkippedMicroOps();
  return StepResult{true, TickResult{0, TickStopReason::kContinue}};
}

TickResult CPU::PerformBusAction(MicroBusAction action, [[maybe_unused]] uint8_t params,
                                 TimeMasterDeltaT cycle_time) {
  namespace mp = opcode_defs_internal::micro_op_params;
  switch (action) {
    case MicroBusAction::kNone:
      return TickResult{0, TickStopReason::kContinue};
    case MicroBusAction::kFetchPc: {
      TickResult blocked = BusRead(PcAddr(regs_), cycle_time);
      if (blocked.reason != TickStopReason::kContinue) return blocked;
      regs_.PC = static_cast<uint16_t>(regs_.PC + 1U);
      return TickResult{0, TickStopReason::kContinue};
    }
    case MicroBusAction::kReadAddr:
      return BusRead(addr_, cycle_time);
    case MicroBusAction::kWriteRegByte: {
      const WriteSrc src = mp::UnpackWriteAddrSrc(params);
      const ByteSel sel = mp::UnpackWriteAddrByteSel(params);
      uint8_t byte = 0;
      switch (src) {
        case WriteSrc::kFetchData:
          byte = fetch_data_;
          break;
        case WriteSrc::kA:
          byte = (sel == ByteSel::kLow) ? static_cast<uint8_t>(regs_.A) : static_cast<uint8_t>(regs_.A >> 8U);
          break;
        case WriteSrc::kX:
          byte = (sel == ByteSel::kLow) ? static_cast<uint8_t>(regs_.X) : static_cast<uint8_t>(regs_.X >> 8U);
          break;
        case WriteSrc::kY:
          byte = (sel == ByteSel::kLow) ? static_cast<uint8_t>(regs_.Y) : static_cast<uint8_t>(regs_.Y >> 8U);
          break;
      }
      return BusWrite(addr_, byte, cycle_time);
    }
    case MicroBusAction::kPushStack: {
      uint8_t byte = 0;
      switch (mp::UnpackPushStack(params)) {
        case PushSrc::kA8:       byte = static_cast<uint8_t>(regs_.A); break;
        case PushSrc::kAHigh:    byte = static_cast<uint8_t>(regs_.A >> 8U); break;
        case PushSrc::kX8:       byte = static_cast<uint8_t>(regs_.X); break;
        case PushSrc::kXHigh:    byte = static_cast<uint8_t>(regs_.X >> 8U); break;
        case PushSrc::kY8:       byte = static_cast<uint8_t>(regs_.Y); break;
        case PushSrc::kYHigh:    byte = static_cast<uint8_t>(regs_.Y >> 8U); break;
        case PushSrc::kPcl:      byte = static_cast<uint8_t>(regs_.PC); break;
        case PushSrc::kPch:      byte = static_cast<uint8_t>(regs_.PC >> 8U); break;
        case PushSrc::kPbr:      byte = regs_.PBR; break;
        case PushSrc::kDbr:      byte = regs_.DBR; break;
        case PushSrc::kP:        byte = regs_.P.ToByte(); break;
        case PushSrc::kDpLow:    byte = static_cast<uint8_t>(regs_.DP); break;
        case PushSrc::kDpHigh:   byte = static_cast<uint8_t>(regs_.DP >> 8U); break;
        case PushSrc::kAddrLow:  byte = static_cast<uint8_t>(addr_ & 0xFFU); break;
        case PushSrc::kAddrHigh: byte = static_cast<uint8_t>((addr_ >> 8U) & 0xFFU); break;
      }
      return BusWrite(StackAddr(regs_), byte, cycle_time);
    }
    case MicroBusAction::kPullStack:
      return BusRead(StackAddr(regs_), cycle_time);
    case MicroBusAction::kPreIncPullStack:
      // Stack pulls: SP must point at the top of the stack before reading.
      if (regs_.P.E) {
        const uint8_t sp_lo = static_cast<uint8_t>(static_cast<uint8_t>(regs_.SP) + 1U);
        regs_.SP = static_cast<uint16_t>(0x0100U | sp_lo);
      } else {
        regs_.SP = static_cast<uint16_t>(regs_.SP + 1U);
      }
      return BusRead(StackAddr(regs_), cycle_time);
  }
  return TickResult{0, TickStopReason::kContinue};
}

CPU::StepResult CPU::ExecuteMicroOp(TimeMasterDeltaT cycle_time) {
  assert(current_instr_ != nullptr);
  // Rules have already been drained by the previous FetchOpcode /
  // ExecuteMicroOp call that transitioned us here; draining again would
  // re-evaluate them for no benefit.
  const InstructionEntry* instr = current_instr_;
  const uint8_t op_idx = static_cast<uint8_t>(micro_op_index_ - 1U);
  assert(op_idx < instr->remaining_op_count);

  const MicroOp* const ops = instr->ops.data();
  const MicroOp& mop = ops[op_idx];
  const SnesAddrT pre_pc = PcAddr(regs_);
  TickResult blocked = PerformBusAction(mop.bus_action, mop.params, cycle_time);
  if (blocked.reason != TickStopReason::kContinue) {
    return StepResult{false, blocked};
  }
  ExecuteInternalOp(mop.internal_op, mop.params);

  if (micro_op_recorder_ != nullptr) {
    MicroOpRecord rec;
    rec.index = micro_op_index_;
    rec.bus_action = mop.bus_action;
    rec.internal_op = mop.internal_op;
    rec.params = mop.params;
    rec.status = MicroOpStatus::kExecuted;
    rec.fetch_data = fetch_data_;
    rec.addr = addr_;
    rec.has_bus = (mop.bus_action != MicroBusAction::kNone);
    rec.bus_addr = (mop.bus_action == MicroBusAction::kFetchPc) ? pre_pc : addr_;
    rec.bus_value = fetch_data_;
    micro_op_recorder_->OnMicroOp(rec);
  }

  ++micro_op_index_;
  if (micro_op_index_ - 1U >= instr->remaining_op_count) {
    FinishInstruction();
  } else {
    DrainSkippedMicroOps();
  }
  return StepResult{true, TickResult{0, TickStopReason::kContinue}};
}

TickResult CPU::Tick(TimeMasterDeltaT budget) {
  if (fault_.has_value()) {
    return {0, TickStopReason::kFaulted};
  }

  TimeMasterDeltaT cycle_time = 0;

  while (cycle_time < budget) {
    // DRAM refresh stalls the CPU mid-scanline. When the refresh window is
    // active, consume cycles without issuing any bus ops. When we cross the
    // scheduled refresh start, arm the window and arrange the next one.
    if (refresh_cycles_remaining_ > 0) {
      const TimeMasterDeltaT available = budget - cycle_time;
      const TimeMasterDeltaT take =
          (refresh_cycles_remaining_ < available) ? refresh_cycles_remaining_ : available;
      cycle_time += take;
      refresh_cycles_remaining_ -= take;
      retired_refresh_cycles_ += take;
      continue;
    }
    if (local_time_ + cycle_time >= next_refresh_time_) {
      refresh_cycles_remaining_ = kDramRefreshDurationCycles;
      next_refresh_time_ += kMasterCyclesPerScanline;
      ++retired_refresh_windows_;
      continue;
    }

    StepResult step = ShouldFetchInstruction() ? FetchOpcode(cycle_time) : ExecuteMicroOp(cycle_time);
    if (step.stop.reason != TickStopReason::kContinue) {
      return step.stop;
    }
    if (step.consumed_cycle) {
      ++cycle_time;
      if (ShouldFetchInstruction() && debugger_contract_.step_target > 0) {
        --debugger_contract_.step_target;
        if (debugger_contract_.step_target == 0) {
          last_debugger_stop_ = TickStopReason::kDebuggerStepComplete;
          return {cycle_time, TickStopReason::kDebuggerStepComplete};
        }
      }
    }
  }

  return {cycle_time, TickStopReason::kBudgetExhausted};
}

void CPU::OnEvent(const SchedulerEvent& /*event*/) {
  // CommitComplete/WakeSample do not currently require CPU-side mutation.
  // The scheduler wakes blocked CPU runs by replacing the authoritative Run
  // wake.
}

}  // namespace pupsnes
