#include "pupsnes/hw/5a22/cpu.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <stdexcept>

#include "pupsnes/core/snes.h"
#include "pupsnes/hw/5a22/cpu_bcd_internal.h"
#include "pupsnes/hw/5a22/cpu_internal.h"
#include "pupsnes/hw/5a22/cpu_mmio.h"
#include "pupsnes/hw/5a22/cpu_opcode_defs_internal.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/memory/systembus.h"

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

// When e=1 the m and x flags are forced to 1, and the stack is forced onto
// page 1 (SH = $01). Independently, whenever the x flag is set — by E=1, by
// SEP/PLP/RTI in native mode, or by XCE entering emulation — X.H and Y.H are
// forced to $00 (Bruce Clark §6.13). Called after any op that can change P or
// e.
[[gnu::always_inline]] inline void ApplyEmulationForcing(CpuRegs& regs) {
  if (regs.P.E) {
    regs.P.M = true;
    regs.P.X = true;
    regs.SP = static_cast<uint16_t>(0x0100U | (regs.SP & 0x00FFU));
  }
  if (regs.P.X) {
    regs.X &= 0x00FFU;
    regs.Y &= 0x00FFU;
  }
}

// Mutable reference to the 16-bit GPR slot selected by `reg`. Used by
// kLoadReg / kIncDecReg / kTransferReg to share the read-modify-write pattern
// across A, X, Y, SP, and DP. Not valid for Reg::kDbr/kP — those are byte-sized
// and handled inline at the call site. PC/PBR have dedicated micro-ops
// (kLoadPcLowFromFetch / kLoadPcHighFromFetch / kLoadPbrFromFetch).
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

CPU::CPU(SNES* snes) : MasterClockDriver(snes) {}

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
  addr_scratch_ = 0;
  timing_context_ = TimingContext{};
  fault_.reset();
  halt_state_ = HaltState::kNone;
  wai_wake_cycles_remaining_ = 0;
  nmi_curr_ = false;
  nmi_gated_prev_ = false;
  nmi_pending_ = false;
  abort_pending_ = false;
  irq_line_asserted_ = false;
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
  partial_op_cycles_ = 0;

  const uint8_t vector_lo = ReadResetVectorByte(0x00FFFCU);
  const uint8_t vector_hi = ReadResetVectorByte(0x00FFFDU);

  regs_.PBR = 0;
  regs_.PC = static_cast<uint16_t>(static_cast<uint16_t>(vector_hi) << 8U) | vector_lo;
}

void CPU::SampleInterrupts(TimeMasterT t) {
  // Sample the PPU /NMI pin. The PPU catches up internally; the returned
  // value is the wire level at master time `t`. Apply the NMITIMEN.7 AND-gate
  // so edge detection operates on the gated output (the signal the CPU's
  // internal NMI flip-flop actually sees).
  const bool line = (snes_ != nullptr && snes_->ppu != nullptr) ? snes_->ppu->SampleNmiLine(t) : false;
  const bool gate_open = (snes_ != nullptr && snes_->cpu_mmio != nullptr) ? snes_->cpu_mmio->GetNmiEnable() : false;
  const bool gated = line && gate_open;
  nmi_curr_ = gated;
  if (gated && !nmi_gated_prev_) {
    nmi_pending_ = true;
  }
  nmi_gated_prev_ = gated;

  // /IRQ is a level line driven by CpuMmio's TIMEUP latch (and, in the future,
  // by other peripherals OR'd in). Sample it directly — no edge tracking
  // required, the CPU delivers as long as the level is asserted at an
  // instruction boundary with P.I clear.
  irq_line_asserted_ = (snes_ != nullptr && snes_->cpu_mmio != nullptr) ? snes_->cpu_mmio->SampleIrqLine() : false;
}

std::optional<InterruptKind> CPU::SelectPendingInterrupt() const {
  // Priority per WDC §9: ABORT > NMI > IRQ. ABORT is still placeholder; NMI
  // and IRQ are live.
  // if (abort_pending_) return InterruptKind::kAbort;
  if (nmi_pending_) return InterruptKind::kNmi;
  if (irq_line_asserted_ && !regs_.P.I) return InterruptKind::kIrq;
  return std::nullopt;
}

bool CPU::WaiShouldWake(TimeMasterT t) {
  // WAI wake is driven by RAW pin assertions, independent of NMITIMEN.7 /
  // I-flag gating (per WDC §18). Delivery is still gated normally at the
  // post-wake instruction-boundary sample — WAI wakes on masked interrupts
  // and simply resumes the instruction after WAI without entering a handler.
  const bool nmi_raw = (snes_ != nullptr && snes_->ppu != nullptr) ? snes_->ppu->SampleNmiLine(t) : false;
  // /IRQ line driven by CpuMmio's TIMEUP latch (plus any future peripheral
  // IRQ sources OR'd in). WAI wakes on level, even when P.I is set.
  const bool irq_raw = (snes_ != nullptr && snes_->cpu_mmio != nullptr) ? snes_->cpu_mmio->SampleIrqLine() : false;
  // Also honour the edge-latched flip-flop: an NMI edge caught on a prior
  // sample (before V advanced past the VBlank-entry line) must still wake WAI
  // even once the raw line has de-asserted.
  return nmi_pending_ || nmi_raw || abort_pending_ || irq_line_asserted_ || irq_raw;
}

void CPU::OnNmiTimenChanged(uint8_t prev_byte, uint8_t new_byte, TimeMasterT t) {
  constexpr uint8_t kNmiEnableMask = 0x80U;
  const bool prev_enable = (prev_byte & kNmiEnableMask) != 0U;
  const bool new_enable = (new_byte & kNmiEnableMask) != 0U;
  if (prev_enable == new_enable) return;

  const bool line = (snes_ != nullptr && snes_->ppu != nullptr) ? snes_->ppu->SampleNmiLine(t) : false;
  if (!prev_enable && new_enable) {
    // 0→1 transparency quirk: the AND-gate output transitions 0→1 if the
    // raw line is currently asserted. That falling-edge-into-flip-flop sets
    // nmi_pending_ immediately, without waiting for the next instruction
    // boundary sample.
    if (line) {
      nmi_pending_ = true;
    }
    nmi_gated_prev_ = line;  // resync edge tracker to the new gate output.
    nmi_curr_ = line;
  } else {
    // 1→0 cancellation: closing the gate clears the pending flip-flop
    // regardless of whether an NMI was about to be delivered this cycle.
    // See fullsnes $4200: "Disabling NMI will reset the internal NMI
    // request flag, even if NMI was triggered just before."
    nmi_pending_ = false;
    nmi_gated_prev_ = false;
    nmi_curr_ = false;
  }
}

void CPU::FinishInstruction() {
  // Restore SP into page $01 in emulation mode for "new" 65C816 stack
  // instructions. Their pushes/pulls use 16-bit SP math (handled in kModifySp
  // and kPreIncPullStack), so SP can briefly leave the $01xx page during the
  // instruction. Bruce Clark §2688 documents the page-$01 restore at end of
  // instruction (PEA at S=$0100 in E=1 ends with S=$01FE, etc.).
  if (current_instr_ != nullptr && current_instr_->is_new_65816_instruction && regs_.P.E) {
    regs_.SP = static_cast<uint16_t>(0x0100U | static_cast<uint8_t>(regs_.SP));
  }
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

void CPU::DebugInjectFault(uint8_t opcode, SnesAddrT address) {
  RecordFault(Fault::Type::kUnimplementedOpcode, opcode, address);
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
  if (result.WasScheduled()) {
    throw std::logic_error("CPU reset vector fetch cannot block on asynchronous bus access");
  }
  return result.data;
}

TickResult CPU::BusReadSlow(SnesAddrT addr, TimeMasterDeltaT cycle_time) {
  auto plan = snes_->system_bus->Plan(addr, BusAccessType::kRead, 0);
  auto result = snes_->system_bus->Follow(plan, local_time_ + cycle_time, device_id_);
  // Async bus scheduling is gone in the new model; WasScheduled() should not
  // fire, but if it does we treat it as a completed access (no blocking).
  fetch_data_ = result.data;
  return TickResult{0, TickStopReason::kReachedTarget};
}

TickResult CPU::BusWriteSlow(SnesAddrT addr, uint8_t data, TimeMasterDeltaT cycle_time) {
  auto plan = snes_->system_bus->Plan(addr, BusAccessType::kWrite, data);
  auto result = snes_->system_bus->Follow(plan, local_time_ + cycle_time, device_id_);
  (void)result;
  return TickResult{0, TickStopReason::kReachedTarget};
}

// BusRead / BusWrite / EvaluateTimingRule are small hot-path member methods
// whose only callers live in this TU. Forcing inlining here recovers the
// inlining the header-inline definitions used to provide — same-TU visibility
// lets the compiler honor the attribute without ODR concerns.
[[gnu::always_inline]] inline TickResult CPU::BusRead(SnesAddrT addr, TimeMasterDeltaT cycle_time) {
  uint8_t data;
  if (system_bus_raw_ != nullptr && system_bus_raw_->TryFastRead(addr, local_time_ + cycle_time, data)) {
    fetch_data_ = data;
    return TickResult{0, TickStopReason::kReachedTarget};
  }
  return BusReadSlow(addr, cycle_time);
}

[[gnu::always_inline]] inline TickResult CPU::BusWrite(SnesAddrT addr, uint8_t data, TimeMasterDeltaT cycle_time) {
  if (system_bus_raw_ != nullptr && system_bus_raw_->TryFastWrite(addr, local_time_ + cycle_time, data)) {
    return TickResult{0, TickStopReason::kReachedTarget};
  }
  return BusWriteSlow(addr, data, cycle_time);
}

[[gnu::always_inline]] inline bool CPU::EvaluateTimingRule(uint32_t truth_table) const {
  const uint32_t bits =
      (static_cast<uint32_t>(timing_context_.branch_taken) << static_cast<uint8_t>(TimingCondition::kBranchTaken)) |
      (static_cast<uint32_t>(IsAccumulator16Bit(regs_)) << static_cast<uint8_t>(TimingCondition::kAccumulator16)) |
      (static_cast<uint32_t>(IsIndex16Bit(regs_)) << static_cast<uint8_t>(TimingCondition::kIndex16)) |
      (static_cast<uint32_t>(regs_.P.E) << static_cast<uint8_t>(TimingCondition::kEmulationMode)) |
      (static_cast<uint32_t>(timing_context_.branch_page_crossed || timing_context_.dp_low_nonzero ||
                             timing_context_.indexed_page_crossed)
       << static_cast<uint8_t>(TimingCondition::kBranchPageCrossed));
  return ((truth_table >> bits) & 1U) != 0U;
}

void CPU::ExecuteInternalOp(MicroInternalOp op, [[maybe_unused]] uint8_t params) {
  namespace mp = opcode_defs_internal::micro_op_params;
  switch (op) {
    case MicroInternalOp::kNone: return;

    case MicroInternalOp::kLoadReg: {
      const Reg reg = mp::UnpackLoadRegReg(params);
      const ByteSel sel = mp::UnpackLoadRegByteSel(params);
      const bool nz = mp::UnpackLoadRegNz(params);
      const uint8_t fetch = fetch_data_;

      if (reg == Reg::kP) {
        regs_.P.FromByte(fetch, regs_.P.E);
        ApplyEmulationForcing(regs_);
        return;
      }
      if (reg == Reg::kDbr) {
        regs_.DBR = fetch;
        regs_.P.Z = (fetch == 0U);
        regs_.P.N = (fetch & 0x80U) != 0U;
        return;
      }

      uint16_t& r = RegRef(regs_, reg);
      const bool high = (sel == ByteSel::kHigh);
      const unsigned shift = high ? 8U : 0U;
      const uint32_t keep = high ? 0x00FFU : 0xFF00U;
      r = static_cast<uint16_t>((r & keep) | (uint32_t{fetch} << shift));
      // NZ: on high, A/X/Y/DP always update (16-bit). On low, DP never updates;
      // A/X/Y update only when nz is set (8-bit).
      const bool skip_nz = !high && (reg == Reg::kDp || !nz);
      if (!skip_nz) SetNzFromWidth(regs_, r, high);
      if (mp::UnpackLoadRegPostIncAddr(params)) {
        addr_ = (addr_ + 1U) & 0xFFFFFFU;
      }
      return;
    }

    case MicroInternalOp::kLoadPcLowFromFetch: {
      regs_.PC = static_cast<uint16_t>((regs_.PC & 0xFF00U) | uint32_t{fetch_data_});
      return;
    }

    case MicroInternalOp::kLoadPcHighFromFetch: {
      regs_.PC = static_cast<uint16_t>((regs_.PC & 0x00FFU) | (uint32_t{fetch_data_} << 8U));
      return;
    }

    case MicroInternalOp::kLoadPbrFromFetch: {
      regs_.PBR = fetch_data_;
      return;
    }

    case MicroInternalOp::kSetBranchTakenCond: {
      const BranchCond cond = mp::UnpackBranchCond(params);
      bool t = (cond == BranchCond::kAlways);
      if (cond == BranchCond::kZ) t = regs_.P.Z;
      if (cond == BranchCond::kNotZ) t = !regs_.P.Z;
      if (cond == BranchCond::kC) t = regs_.P.C;
      if (cond == BranchCond::kNotC) t = !regs_.P.C;
      if (cond == BranchCond::kN) t = regs_.P.N;
      if (cond == BranchCond::kNotN) t = !regs_.P.N;
      if (cond == BranchCond::kV) t = regs_.P.V;
      if (cond == BranchCond::kNotV) t = !regs_.P.V;
      timing_context_.branch_taken = t;
      return;
    }

    case MicroInternalOp::kBranchRelative: {
      // BRL uses a 16-bit signed displacement stashed in addr_[15:0]; the 8-bit
      // path also updates branch_page_crossed.
      const bool wide = mp::UnpackBranchRelativeWide(params);
      const int32_t displacement =
          wide ? static_cast<int16_t>(static_cast<uint16_t>(addr_ & 0xFFFFU)) : static_cast<int8_t>(fetch_data_);
      const uint16_t old_pc = regs_.PC;
      regs_.PC = static_cast<uint16_t>(static_cast<int32_t>(old_pc) + displacement);
      if (!wide) timing_context_.branch_page_crossed = ((old_pc ^ regs_.PC) & 0xFF00U) != 0U;
      return;
    }

    case MicroInternalOp::kSetAddrFromDp: {
      // In emulation mode with DL=0, direct-page operand addition wraps within
      // the DP page (bank 0, DH fixed): matches 6502 "zero-page" wrap. In
      // native mode — or when DL != 0 — the full 16-bit add in bank 0 applies.
      if (regs_.P.E && (regs_.DP & 0x00FFU) == 0U) {
        const uint32_t page = static_cast<uint32_t>(regs_.DP) & 0xFF00U;
        const uint32_t offset = (static_cast<uint32_t>(regs_.DP) + static_cast<uint32_t>(fetch_data_)) & 0x00FFU;
        addr_ = page | offset;
      } else {
        addr_ = (static_cast<uint32_t>(regs_.DP) + static_cast<uint32_t>(fetch_data_)) & 0x0000FFFFU;
      }
      return;
    }

    case MicroInternalOp::kSetAddrFromSp: {
      addr_ = (static_cast<uint32_t>(regs_.SP) + static_cast<uint32_t>(fetch_data_)) & 0x0000FFFFU;
      return;
    }

    case MicroInternalOp::kStashIndirectLow: {
      addr_scratch_ = static_cast<uint16_t>((addr_scratch_ & 0xFF00U) | fetch_data_);
      // Pointer fetches stay inside the current bank — an overflow at $xxFFFF
      // wraps to $xx0000, not $(xx+1)0000. Bank 0 pointers (DP/abs-indirect)
      // would otherwise leak into bank 1 at end-of-bank.
      addr_ = (addr_ & 0x00FF0000U) | ((addr_ + 1U) & 0x0000FFFFU);
      return;
    }

    case MicroInternalOp::kStashIndirectHigh: {
      const uint32_t high = static_cast<uint32_t>(fetch_data_) << 8U;
      addr_scratch_ = static_cast<uint16_t>((addr_scratch_ & 0x00FFU) | high);
      addr_ = (addr_ & 0x00FF0000U) | ((addr_ + 1U) & 0x0000FFFFU);
      return;
    }

    case MicroInternalOp::kStashOperandLow: {
      // 16-bit data fetch through the effective address: stash the low byte
      // into scratch and advance addr_ with 24-bit carry. Required when the
      // operand straddles a bank boundary — e.g. ADC $FFFF / ASL $FFFF with
      // DBR=$7E must read the high byte from $7F:0000, not $7E:0000.
      addr_scratch_ = static_cast<uint16_t>((addr_scratch_ & 0xFF00U) | fetch_data_);
      addr_ = (addr_ + 1U) & 0x00FFFFFFU;
      return;
    }

    case MicroInternalOp::kStashDpIndirectLow: {
      // (dp) and (dp),Y pointer advance. The pointer fetch wraps within the
      // 256-byte page in E=1 ONLY when DPL=$00 (cputest-full 0034 with DPL=0
      // wraps; 0035 with DPL=$01 does not). Otherwise it's a normal bank-wrap
      // +1. The (dp,X) form uses kStashDpXIndirectLow instead — it wraps
      // unconditionally in E=1. PEI is a "new" 65C816 instruction and skips
      // the DP-page wrap even with E=1 + DPL=$00 (Bruce Clark §5.1.1, witness
      // cputest-full test 0x03C4: PEI $FF with D=$0200 reads pointer high
      // from $0300 rather than wrapping back to $0200).
      addr_scratch_ = static_cast<uint16_t>((addr_scratch_ & 0xFF00U) | fetch_data_);
      const bool is_new_instr = (current_instr_ != nullptr) && current_instr_->is_new_65816_instruction;
      if (regs_.P.E && (regs_.DP & 0x00FFU) == 0U && !is_new_instr) {
        addr_ = (addr_ & 0x00FFFF00U) | ((addr_ + 1U) & 0x000000FFU);
      } else {
        addr_ = (addr_ & 0x00FF0000U) | ((addr_ + 1U) & 0x0000FFFFU);
      }
      return;
    }

    case MicroInternalOp::kStashDpXIndirectLow: {
      // (dp,X) pointer advance. The X-add cycle already operated in 8-bit
      // mode (E=1 + dp_wrap), and the pointer fetch inherits that — wrapping
      // within the 256-byte page even when DPL≠$00. Hardware witness:
      // cputest-full test 0027 (`ADC ($F7,X)`, D=$011A, DPL=$1A, E=1) reads
      // pointer high at $0200 after pointer low at $02FF.
      addr_scratch_ = static_cast<uint16_t>((addr_scratch_ & 0xFF00U) | fetch_data_);
      if (regs_.P.E) {
        addr_ = (addr_ & 0x00FFFF00U) | ((addr_ + 1U) & 0x000000FFU);
      } else {
        addr_ = (addr_ & 0x00FF0000U) | ((addr_ + 1U) & 0x0000FFFFU);
      }
      return;
    }

    case MicroInternalOp::kFormAddrFromScratchDbr: {
      const uint32_t low = static_cast<uint32_t>(addr_scratch_ & 0x00FFU);
      const uint32_t high = static_cast<uint32_t>(fetch_data_) << 8U;
      const uint32_t bank = static_cast<uint32_t>(regs_.DBR) << 16U;
      addr_ = bank | high | low;
      return;
    }

    case MicroInternalOp::kFormAddrFromScratchBank: {
      const uint32_t low = static_cast<uint32_t>(addr_scratch_ & 0x00FFU);
      const uint32_t high = static_cast<uint32_t>(addr_scratch_ & 0xFF00U);
      const uint32_t bank = static_cast<uint32_t>(fetch_data_) << 16U;
      addr_ = bank | high | low;
      if (mp::UnpackFormAddrFromScratchBankWithYAdd(params)) {
        const uint16_t index = IsIndex16Bit(regs_) ? regs_.Y : static_cast<uint16_t>(regs_.Y & 0x00FFU);
        addr_ = (addr_ + index) & 0x00FFFFFFU;
      }
      return;
    }

    case MicroInternalOp::kAddIndexToAddr: {
      const Reg reg = mp::UnpackAddIndex(params);
      const uint16_t index = (reg == Reg::kY) ? regs_.Y : regs_.X;
      const uint16_t masked = IsIndex16Bit(regs_) ? index : static_cast<uint16_t>(index & 0x00FFU);
      if (mp::UnpackAddIndexBankWrap(params)) {
        // Preserve the original bank byte; wrap the add within the existing
        // bank. Required by direct-page-indexed (bank always 0) and by
        // (abs,X) indirect (bank = PBR — the pointer fetch stays in the
        // program bank even if the low 16 bits overflow).
        const uint32_t bank = addr_ & 0x00FF0000U;
        if (mp::UnpackAddIndexDpWrap(params) && regs_.P.E) {
          // (dp,X) in emulation mode: 8-bit page wrap. The index add is done
          // on the low byte only — the high byte and bank stay fixed. Holds
          // regardless of DPL (witness: cputest-full test 0025 with DPL=$00
          // *and* test 0026/0027 with DPL=$1A both expect this).
          const uint32_t hi = addr_ & 0x0000FF00U;
          const uint32_t lo = (addr_ + static_cast<uint32_t>(masked)) & 0x000000FFU;
          addr_ = bank | hi | lo;
        } else {
          addr_ = bank | ((addr_ + static_cast<uint32_t>(masked)) & 0x0000FFFFU);
        }
      } else {
        addr_ = (addr_ + static_cast<uint32_t>(masked)) & 0x00FFFFFFU;
      }
      return;
    }

    case MicroInternalOp::kSetAddrHighDbrAddIndex: {
      // Place the just-fetched high byte and DBR bank, then add the index with
      // 24-bit carry — the absolute-indexed read effective address is
      // DBR:(abs + idx). Folding the add into this fetch cycle keeps the
      // no-page-cross case one cycle cheaper than indexed stores; the extra
      // cycle is billed by a following slot gated on kIndexedPageCrossed.
      addr_ =
          (addr_ & 0x0000FFU) | (static_cast<uint32_t>(fetch_data_) << 8U) | (static_cast<uint32_t>(regs_.DBR) << 16U);
      const Reg reg = mp::UnpackAddIndex(params);
      const uint16_t index = (reg == Reg::kY) ? regs_.Y : regs_.X;
      const uint16_t masked = IsIndex16Bit(regs_) ? index : static_cast<uint16_t>(index & 0x00FFU);
      const uint16_t base16 = static_cast<uint16_t>(addr_ & 0x00FFFFU);
      const uint16_t sum16 = static_cast<uint16_t>(base16 + masked);
      timing_context_.indexed_page_crossed = (base16 & 0xFF00U) != (sum16 & 0xFF00U);
      addr_ = (addr_ + static_cast<uint32_t>(masked)) & 0x00FFFFFFU;
      return;
    }

    case MicroInternalOp::kSetAddrBankFromFetchAddX: {
      // Absolute-long-indexed-X: set the explicit bank byte, then add X with
      // 24-bit carry. No page-cross penalty exists (bank is explicit), so the
      // add is folded into the bank fetch — long,X is a flat 6-m cycles.
      addr_ = (addr_ & 0x00FFFFU) | (static_cast<uint32_t>(fetch_data_) << 16U);
      const uint16_t index = IsIndex16Bit(regs_) ? regs_.X : static_cast<uint16_t>(regs_.X & 0x00FFU);
      addr_ = (addr_ + static_cast<uint32_t>(index)) & 0x00FFFFFFU;
      return;
    }

    case MicroInternalOp::kSetAddrByteFromFetch: {
      const ByteSel sel = mp::UnpackSetAddrByteSel(params);
      const unsigned shift = (sel == ByteSel::kLow) ? 0U : (sel == ByteSel::kHigh) ? 8U : 16U;
      const uint32_t mask = ~(uint32_t{0xFFU} << shift) & 0xFFFFFFU;
      addr_ = (addr_ & mask) | (static_cast<uint32_t>(fetch_data_) << shift);
      // BankSrc is only meaningful with kHigh; the bank byte is written
      // alongside the high byte when a source other than kLeave is requested.
      if (sel == ByteSel::kHigh) {
        const BankSrc bank_src = mp::UnpackSetAddrBankSrc(params);
        switch (bank_src) {
          case BankSrc::kLeave: break;
          case BankSrc::kDbr: addr_ = (addr_ & 0x00FFFFU) | (static_cast<uint32_t>(regs_.DBR) << 16U); break;
          case BankSrc::kPbr: addr_ = (addr_ & 0x00FFFFU) | (static_cast<uint32_t>(regs_.PBR) << 16U); break;
          case BankSrc::kZero: addr_ = addr_ & 0x00FFFFU; break;
        }
      }
      return;
    }

    case MicroInternalOp::kModifyAddr: {
      const uint32_t delta = mp::UnpackModifyAddrIncrement(params) ? 1U : UINT32_C(0xFFFFFF);
      addr_ = (addr_ + delta) & 0xFFFFFFU;
      return;
    }

    case MicroInternalOp::kModifySp: {
      const uint16_t delta = mp::UnpackModifySpIncrement(params) ? 0x0001U : 0xFFFFU;
      // "New" 65C816 stack instructions in emulation mode use 16-bit SP math
      // during pushes/pulls — no wrap at the $01xx page boundary — and the
      // high byte is restored to $01 at end-of-instruction (handled in
      // FinishInstruction). For "old" 6502-compatible push/pull (PHA/PHP/PLA/
      // PLP/JSR abs/RTS/BRK/COP/IRQ/NMI/ABORT), keep the 8-bit page-1 wrap.
      const bool wide = (current_instr_ != nullptr) && current_instr_->is_new_65816_instruction;
      if (regs_.P.E && !wide) {
        regs_.SP = static_cast<uint16_t>(0x0100U | static_cast<uint8_t>(regs_.SP + delta));
      } else {
        regs_.SP = static_cast<uint16_t>(regs_.SP + delta);
      }
      return;
    }

    case MicroInternalOp::kModifyPc:
      regs_.PC = static_cast<uint16_t>(regs_.PC + (mp::UnpackModifyPcIncrement(params) ? 1U : 0xFFFFU));
      return;

    case MicroInternalOp::kIncDecReg: {
      const Reg reg = mp::UnpackIncDecReg(params);
      const uint16_t delta = mp::UnpackIncDecDecrement(params) ? 0xFFFFU : 0x0001U;
      const bool wide = (reg == Reg::kA) ? IsAccumulator16Bit(regs_) : IsIndex16Bit(regs_);
      uint16_t& r = RegRef(regs_, reg);
      r = wide ? static_cast<uint16_t>(r + delta)
               : static_cast<uint16_t>((r & 0xFF00U) | static_cast<uint8_t>(r + delta));
      SetNzFromWidth(regs_, r, wide);
      return;
    }

    case MicroInternalOp::kSetFlag: {
      const bool v = mp::UnpackSetFlagValue(params);
      const Flag f = mp::UnpackSetFlagFlag(params);
      if (f == Flag::kC) regs_.P.C = v;
      if (f == Flag::kD) regs_.P.D = v;
      if (f == Flag::kI) regs_.P.I = v;
      if (f == Flag::kV) regs_.P.V = v;
      return;
    }

    case MicroInternalOp::kMaskStatus: {
      const uint8_t p = regs_.P.ToByte();
      const uint8_t next = mp::UnpackMaskStatusOr(params) ? static_cast<uint8_t>(p | fetch_data_)
                                                          : static_cast<uint8_t>(p & ~fetch_data_);
      regs_.P.FromByte(next, regs_.P.E);
      ApplyEmulationForcing(regs_);
      return;
    }

    case MicroInternalOp::kExchangeCarryEmulation: {
      const bool tmp = regs_.P.C;
      regs_.P.C = regs_.P.E;
      regs_.P.E = tmp;
      ApplyEmulationForcing(regs_);
      return;
    }

    case MicroInternalOp::kSwapBA: {
      regs_.A = static_cast<uint16_t>(((regs_.A & 0x00FFU) << 8U) | ((regs_.A & 0xFF00U) >> 8U));
      const uint8_t low = static_cast<uint8_t>(regs_.A & 0x00FFU);
      regs_.P.Z = (low == 0U);
      regs_.P.N = (low & 0x80U) != 0U;
      return;
    }

    case MicroInternalOp::kAddPcToAddr: {
      const uint32_t sum = (static_cast<uint32_t>(regs_.PC) + (addr_ & 0xFFFFU)) & 0xFFFFU;
      addr_ = (addr_ & 0xFF0000U) | sum;
      return;
    }

    case MicroInternalOp::kSetInterruptVector: {
      // Bits [2:0] select InterruptKind. Vectors per WDC §9 / Bruce Clark
      // §6.3.1 §6.11. In emulation mode IRQ and BRK share $00FFFE — the
      // handler distinguishes them by the B flag pushed in P.
      const InterruptKind kind = static_cast<InterruptKind>(params & 0x07U);
      const bool e = regs_.P.E;
      switch (kind) {
        case InterruptKind::kBrk: addr_ = e ? 0x00FFFEU : 0x00FFE6U; break;
        case InterruptKind::kCop: addr_ = e ? 0x00FFF4U : 0x00FFE4U; break;
        case InterruptKind::kNmi: addr_ = e ? 0x00FFFAU : 0x00FFEAU; break;
        case InterruptKind::kIrq: addr_ = e ? 0x00FFFEU : 0x00FFEEU; break;
        case InterruptKind::kAbort: addr_ = e ? 0x00FFF8U : 0x00FFE8U; break;
      }
      return;
    }

    case MicroInternalOp::kEnterInterruptHandler: {
      const uint16_t scratch_low = static_cast<uint16_t>(addr_scratch_ & 0x00FFU);
      regs_.PC = static_cast<uint16_t>((static_cast<uint16_t>(fetch_data_) << 8U) | scratch_low);
      regs_.PBR = 0;
      regs_.P.I = true;
      regs_.P.D = false;
      return;
    }

    case MicroInternalOp::kHaltCpu: {
      const bool is_stp = (params & 0x01U) != 0U;
      halt_state_ = is_stp ? HaltState::kStp : HaltState::kWai;
      wai_wake_cycles_remaining_ = 0;
      return;
    }

    case MicroInternalOp::kMoveSetDbr: regs_.DBR = fetch_data_; return;

    case MicroInternalOp::kMoveSetAddrFromSrc:
      addr_ = (static_cast<uint32_t>(fetch_data_) << 16U) | static_cast<uint32_t>(regs_.X & 0xFFFFU);
      return;

    case MicroInternalOp::kMoveSetAddrFromDst:
      addr_ = (static_cast<uint32_t>(regs_.DBR) << 16U) | static_cast<uint32_t>(regs_.Y & 0xFFFFU);
      return;

    case MicroInternalOp::kMoveAdjust: {
      // Shared params with kWriteRegByte: WriteSrc lives in bits [2:0] and
      // ByteSel in bit 3. Use bit 4 for the direction flag so kWriteRegByte
      // still sees WriteSrc::kFetchData (0x00).
      const bool dec = (params & 0x10U) != 0U;
      regs_.A = static_cast<uint16_t>((regs_.A - 1U) & 0xFFFFU);
      const uint32_t delta = dec ? 0xFFFFU : 0x0001U;  // -1 or +1 mod 65536
      if (IsIndex16Bit(regs_)) {
        regs_.X = static_cast<uint16_t>((regs_.X + delta) & 0xFFFFU);
        regs_.Y = static_cast<uint16_t>((regs_.Y + delta) & 0xFFFFU);
      } else {
        regs_.X = static_cast<uint16_t>((regs_.X & 0xFF00U) | ((static_cast<uint32_t>(regs_.X) + delta) & 0x00FFU));
        regs_.Y = static_cast<uint16_t>((regs_.Y & 0xFF00U) | ((static_cast<uint32_t>(regs_.Y) + delta) & 0x00FFU));
      }
      return;
    }

    case MicroInternalOp::kMoveLoopCheck:
      if (regs_.A != 0xFFFFU) {
        regs_.PC = static_cast<uint16_t>((regs_.PC - 3U) & 0xFFFFU);
      }
      return;

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
          (dst == Reg::kA ? (src == Reg::kDp || src == Reg::kSp || IsAccumulator16Bit(regs_)) : IsIndex16Bit(regs_));
      uint16_t& d = RegRef(regs_, dst);
      const uint16_t s = RegRef(regs_, src);
      d = wide ? s : static_cast<uint16_t>((d & 0xFF00U) | (s & 0x00FFU));
      SetNzFromWidth(regs_, d, wide);
      return;
    }

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

    case MicroInternalOp::kSetPcFromScratchAndFetch: {
      // Final cycle of indirect-jump addressing modes. The prior cycles
      // stashed pointer bytes into addr_scratch_; this cycle's kReadAddr
      // filled fetch_data_ with the final pointer byte. with_pbr = true for
      // [abs] (24-bit pointer — PBR gets fetch_data_, PC gets scratch
      // [15:0]); with_pbr = false for (abs) / (abs,X) (16-bit pointer — PC
      // gets fetch_data_:scratch_low, PBR unchanged).
      const bool with_pbr = (params & 0x01U) != 0U;
      if (with_pbr) {
        regs_.PC = static_cast<uint16_t>(addr_scratch_ & 0xFFFFU);
        regs_.PBR = fetch_data_;
      } else {
        const uint16_t scratch_low = static_cast<uint16_t>(addr_scratch_ & 0x00FFU);
        regs_.PC = static_cast<uint16_t>((static_cast<uint16_t>(fetch_data_) << 8U) | scratch_low);
      }
      return;
    }

    case MicroInternalOp::kShiftRotateA: {
      const ShiftOp sop = static_cast<ShiftOp>(params & 0x03U);
      const bool wide = IsAccumulator16Bit(regs_);
      const uint16_t mask = wide ? 0xFFFFU : 0x00FFU;
      const uint16_t sign = wide ? 0x8000U : 0x0080U;
      const uint16_t a_keep = wide ? 0x0000U : 0xFF00U;
      const uint16_t a_val = regs_.A & mask;
      uint16_t result = 0;
      bool carry_out = false;
      switch (sop) {
        case ShiftOp::kAsl:
          carry_out = (a_val & sign) != 0U;
          result = static_cast<uint16_t>((static_cast<uint32_t>(a_val) << 1U) & mask);
          break;
        case ShiftOp::kLsr:
          carry_out = (a_val & 0x0001U) != 0U;
          result = static_cast<uint16_t>(static_cast<uint32_t>(a_val) >> 1U);
          break;
        case ShiftOp::kRol:
          carry_out = (a_val & sign) != 0U;
          result = static_cast<uint16_t>(((static_cast<uint32_t>(a_val) << 1U) | (regs_.P.C ? 1U : 0U)) & mask);
          break;
        case ShiftOp::kRor:
          carry_out = (a_val & 0x0001U) != 0U;
          result = static_cast<uint16_t>((static_cast<uint32_t>(a_val) >> 1U) |
                                         (regs_.P.C ? static_cast<uint32_t>(sign) : 0U));
          break;
      }
      regs_.P.C = carry_out;
      regs_.P.Z = (result == 0U);
      regs_.P.N = (result & sign) != 0U;
      regs_.A = static_cast<uint16_t>((regs_.A & a_keep) | (result & mask));
      return;
    }

    case MicroInternalOp::kRmwMem: {
      // Memory RMW. Width follows M flag. 8-bit path consumes fetch_data_ (the
      // byte read this instruction), rewrites it in place, and backs addr_ up
      // by 1 so the paired write cycle emits at the original effective
      // address. 16-bit path consumes addr_scratch_[7:0] (low byte, stashed
      // earlier) + fetch_data_ (high byte just read) as a 16-bit operand;
      // writes new_high back to fetch_data_ and new_low back to
      // addr_scratch_[7:0], leaving addr_ at the high-byte address so the
      // paired write emits high-first-then-low.
      const RmwOp rop = static_cast<RmwOp>(params & 0x07U);
      const bool wide = IsAccumulator16Bit(regs_);
      const uint16_t mask = wide ? 0xFFFFU : 0x00FFU;
      const uint16_t sign = wide ? 0x8000U : 0x0080U;
      const uint32_t operand =
          wide ? static_cast<uint32_t>((static_cast<uint32_t>(fetch_data_) << 8U) | (addr_scratch_ & 0x00FFU))
               : static_cast<uint32_t>(fetch_data_);
      uint32_t result = 0;
      bool carry_out = false;
      switch (rop) {
        case RmwOp::kAsl:
          carry_out = (operand & sign) != 0U;
          result = (operand << 1U) & mask;
          break;
        case RmwOp::kLsr:
          carry_out = (operand & 0x0001U) != 0U;
          result = (operand >> 1U) & mask;
          break;
        case RmwOp::kRol:
          carry_out = (operand & sign) != 0U;
          result = ((operand << 1U) | (regs_.P.C ? 1U : 0U)) & mask;
          break;
        case RmwOp::kRor:
          carry_out = (operand & 0x0001U) != 0U;
          result = ((operand >> 1U) | (regs_.P.C ? static_cast<uint32_t>(sign) : 0U)) & mask;
          break;
        case RmwOp::kInc:
          result = (operand + 1U) & mask;
          // INC/DEC do NOT touch C.
          carry_out = regs_.P.C;
          break;
        case RmwOp::kDec:
          result = (operand + mask) & mask;  // +mask == -1 modulo mask+1
          carry_out = regs_.P.C;
          break;
        case RmwOp::kTsb:
        case RmwOp::kTrb: {
          // TSB/TRB: Z reflects (A & mem), then mem' = mem | A (TSB) or
          // mem & ~A (TRB). N and C are preserved. Bruce Clark §6.1.2.3.
          const uint32_t a_masked = static_cast<uint32_t>(regs_.A) & mask;
          regs_.P.Z = ((operand & a_masked) == 0U);
          result = (rop == RmwOp::kTsb) ? ((operand | a_masked) & mask) : ((operand & ~a_masked) & mask);
          if (wide) {
            fetch_data_ = static_cast<uint8_t>((result >> 8U) & 0xFFU);
            addr_scratch_ = static_cast<uint16_t>((addr_scratch_ & 0xFF00U) | (result & 0x00FFU));
          } else {
            fetch_data_ = static_cast<uint8_t>(result & 0xFFU);
            addr_ = (addr_ + 0xFFFFFFU) & 0xFFFFFFU;
          }
          return;
        }
      }
      // Shift/rotate ops update C; INC/DEC leave it alone (carry_out preset).
      if (rop == RmwOp::kAsl || rop == RmwOp::kLsr || rop == RmwOp::kRol || rop == RmwOp::kRor) {
        regs_.P.C = carry_out;
      }
      regs_.P.Z = (result == 0U);
      regs_.P.N = (result & sign) != 0U;

      if (wide) {
        fetch_data_ = static_cast<uint8_t>((result >> 8U) & 0xFFU);
        addr_scratch_ = static_cast<uint16_t>((addr_scratch_ & 0xFF00U) | (result & 0x00FFU));
        // addr_ stays at the high-byte address — paired writes go high, dec, low.
      } else {
        fetch_data_ = static_cast<uint8_t>(result & 0xFFU);
        // Back up addr_ so the paired write cycle (which pairs kWriteRegByte
        // with kModifyAddr(decrement)) lands at the original effective
        // address. Without this roll-back the write would go to addr+1
        // because 8-bit RMW is emitted after a kStashIndirectLow (shared with
        // the 16-bit path) that advanced addr_ past the byte.
        addr_ = (addr_ + 0xFFFFFFU) & 0xFFFFFFU;
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
      const uint16_t low_src = mp::UnpackAluLowFromScratch(params) ? static_cast<uint16_t>(addr_scratch_ & 0xFFU)
                                                                   : static_cast<uint16_t>(addr_ & 0xFFU);
      const uint16_t operand = wide ? static_cast<uint16_t>(low_src | fetch_high) : static_cast<uint16_t>(fetch_data_);
      const uint16_t mask = wide ? 0xFFFFU : 0x00FFU;
      const uint16_t sign = wide ? 0x8000U : 0x0080U;
      const uint32_t carry = wide ? 0x10000U : 0x0100U;
      const AluOp alu = mp::UnpackAluOp(params);
      const uint16_t a_keep = wide ? 0x0000U : 0xFF00U;

      switch (alu) {
        case AluOp::kAdc:
        case AluOp::kSbc: {
          if (regs_.P.D) {
            // BCD decimal mode (D-08). ADC and SBC use different fixup
            // directions (see BcdAdd8/BcdSub8 in cpu_bcd_internal.h +
            // deviation note): ADC adds +6/+$60, SBC subtracts -6/-$60 based
            // on the nibble half-carry and byte full-carry of the raw binary
            // sum.
            BcdResult r;
            if (wide) {
              r = (alu == AluOp::kSbc) ? BcdSub16(regs_.A, operand, regs_.P.C) : BcdAdd16(regs_.A, operand, regs_.P.C);
              regs_.A = r.value;
            } else {
              const uint8_t a_lo = static_cast<uint8_t>(regs_.A & 0xFFU);
              const uint8_t b_lo = static_cast<uint8_t>(operand & 0xFFU);
              r = (alu == AluOp::kSbc) ? BcdSub8(a_lo, b_lo, regs_.P.C) : BcdAdd8(a_lo, b_lo, regs_.P.C);
              regs_.A = static_cast<uint16_t>((regs_.A & a_keep) | (r.value & 0xFFU));
            }
            regs_.P.N = r.negative;
            regs_.P.V = r.overflow;
            regs_.P.Z = r.zero;
            regs_.P.C = r.carry;
            return;
          }
          // Binary path — unchanged from pre-BCD behavior.
          const uint16_t rhs = (alu == AluOp::kSbc) ? static_cast<uint16_t>(~operand & mask) : operand;
          const uint32_t a_val = regs_.A & mask;
          const uint32_t sum = a_val + rhs + (regs_.P.C ? 1U : 0U);
          const uint16_t result = static_cast<uint16_t>(sum & mask);
          regs_.P.V = ((~(a_val ^ rhs) & (a_val ^ result)) & sign) != 0U;
          regs_.P.C = (sum & carry) != 0U;
          regs_.P.Z = (result == 0U);
          regs_.P.N = (result & sign) != 0U;
          regs_.A = static_cast<uint16_t>((regs_.A & a_keep) | (result & mask));
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
          regs_.A = static_cast<uint16_t>((regs_.A & a_keep) | (result & mask));
          return;
        }
        case AluOp::kCmp:
        case AluOp::kCpx:
        case AluOp::kCpy: {
          const uint16_t reg_val = (alu == AluOp::kCmp)   ? static_cast<uint16_t>(regs_.A & mask)
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
        case AluOp::kBitMem: {
          // Memory BIT: N = operand bit 7/15, V = operand bit 6/14, Z = (A & operand == 0).
          // Bruce Clark §6.1.2.2. Distinct from immediate BIT (kBit) which only sets Z.
          // A register is NOT modified.
          const uint16_t v_bit = wide ? 0x4000U : 0x0040U;
          regs_.P.N = (operand & sign) != 0U;
          regs_.P.V = (operand & v_bit) != 0U;
          regs_.P.Z = ((regs_.A & mask & operand) == 0U);
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
      return StepResult{TickStopReason::kBreakpoint, true};
    }
  }

  pending_trace_ = TraceEntry{local_time_ + cycle_time, opcode_address, regs_};

  TickResult bus_result = BusRead(opcode_address, cycle_time);
  if (bus_result.reason != TickStopReason::kReachedTarget) {
    return StepResult{bus_result.reason, true};
  }
  regs_.PC = static_cast<uint16_t>(regs_.PC + 1U);
  timing_context_ = TimingContext{};

  const InstructionEntry& entry = opcode_defs_internal::kOpcodeArtifacts.execution_table[fetch_data_];
  // Seed the DP-low-nonzero bit only for DP-addressed opcodes. Branches share
  // the same truth-table slot (bit 4) but mean branch_page_crossed there, so
  // spurious seeding would fire the emulation-mode page-cross penalty.
  if (entry.uses_dp_penalty) {
    timing_context_.dp_low_nonzero = (regs_.DP & 0x00FFU) != 0U;
  }
  if (entry.disposition == InstructionDisposition::kFaultUnimplemented) {
    RecordFault(Fault::Type::kUnimplementedOpcode, fetch_data_, opcode_address);
    return StepResult{TickStopReason::kFault, true};
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
  return StepResult{TickStopReason::kReachedTarget, false};
}

TickResult CPU::PerformBusAction(MicroBusAction action, [[maybe_unused]] uint8_t params, TimeMasterDeltaT cycle_time) {
  namespace mp = opcode_defs_internal::micro_op_params;
  switch (action) {
    case MicroBusAction::kNone: return TickResult{0, TickStopReason::kReachedTarget};
    case MicroBusAction::kFetchPc: {
      TickResult blocked = BusRead(PcAddr(regs_), cycle_time);
      if (blocked.reason != TickStopReason::kReachedTarget) return blocked;
      regs_.PC = static_cast<uint16_t>(regs_.PC + 1U);
      return TickResult{0, TickStopReason::kReachedTarget};
    }
    case MicroBusAction::kReadAddr: return BusRead(addr_, cycle_time);
    case MicroBusAction::kWriteRegByte: {
      const WriteSrc src = mp::UnpackWriteAddrSrc(params);
      const ByteSel sel = mp::UnpackWriteAddrByteSel(params);
      uint8_t byte = 0;
      switch (src) {
        case WriteSrc::kFetchData: byte = fetch_data_; break;
        case WriteSrc::kA:
          byte = (sel == ByteSel::kLow) ? static_cast<uint8_t>(regs_.A) : static_cast<uint8_t>(regs_.A >> 8U);
          break;
        case WriteSrc::kX:
          byte = (sel == ByteSel::kLow) ? static_cast<uint8_t>(regs_.X) : static_cast<uint8_t>(regs_.X >> 8U);
          break;
        case WriteSrc::kY:
          byte = (sel == ByteSel::kLow) ? static_cast<uint8_t>(regs_.Y) : static_cast<uint8_t>(regs_.Y >> 8U);
          break;
        case WriteSrc::kZero: byte = 0; break;
        case WriteSrc::kScratchLow: byte = static_cast<uint8_t>(addr_scratch_ & 0x00FFU); break;
      }
      return BusWrite(addr_, byte, cycle_time);
    }
    case MicroBusAction::kPushStack: {
      uint8_t byte = 0;
      switch (mp::UnpackPushStack(params)) {
        case PushSrc::kA8: byte = static_cast<uint8_t>(regs_.A); break;
        case PushSrc::kAHigh: byte = static_cast<uint8_t>(regs_.A >> 8U); break;
        case PushSrc::kX8: byte = static_cast<uint8_t>(regs_.X); break;
        case PushSrc::kXHigh: byte = static_cast<uint8_t>(regs_.X >> 8U); break;
        case PushSrc::kY8: byte = static_cast<uint8_t>(regs_.Y); break;
        case PushSrc::kYHigh: byte = static_cast<uint8_t>(regs_.Y >> 8U); break;
        case PushSrc::kPcl: byte = static_cast<uint8_t>(regs_.PC); break;
        case PushSrc::kPch: byte = static_cast<uint8_t>(regs_.PC >> 8U); break;
        case PushSrc::kPbr: byte = regs_.PBR; break;
        case PushSrc::kDbr: byte = regs_.DBR; break;
        case PushSrc::kP: byte = regs_.P.ToByte(); break;
        case PushSrc::kPHwIrq: {
          // HW interrupts (NMI/IRQ/ABORT) push P with the B flag cleared in
          // emulation mode so handlers can distinguish them from BRK (which
          // pushes B=1). In native mode there is no B flag — the bit position
          // carries the X flag — and the byte is pushed as-is.
          uint8_t p = regs_.P.ToByte();
          if (regs_.P.E) p = static_cast<uint8_t>(p & ~0x10U);
          byte = p;
          break;
        }
        case PushSrc::kDpLow: byte = static_cast<uint8_t>(regs_.DP); break;
        case PushSrc::kDpHigh: byte = static_cast<uint8_t>(regs_.DP >> 8U); break;
        case PushSrc::kAddrLow: byte = static_cast<uint8_t>(addr_ & 0xFFU); break;
        case PushSrc::kAddrHigh: byte = static_cast<uint8_t>((addr_ >> 8U) & 0xFFU); break;
      }
      return BusWrite(StackAddr(regs_), byte, cycle_time);
    }
    case MicroBusAction::kPullStack: return BusRead(StackAddr(regs_), cycle_time);
    case MicroBusAction::kPreIncPullStack: {
      // Stack pulls: SP must point at the top of the stack before reading.
      // "New" 65C816 instructions in E=1 use 16-bit SP math (no page-1 wrap);
      // the high byte is restored to $01 at end-of-instruction.
      const bool wide = (current_instr_ != nullptr) && current_instr_->is_new_65816_instruction;
      if (regs_.P.E && !wide) {
        const uint8_t sp_lo = static_cast<uint8_t>(static_cast<uint8_t>(regs_.SP) + 1U);
        regs_.SP = static_cast<uint16_t>(0x0100U | sp_lo);
      } else {
        regs_.SP = static_cast<uint16_t>(regs_.SP + 1U);
      }
      return BusRead(StackAddr(regs_), cycle_time);
    }
  }
  return TickResult{0, TickStopReason::kReachedTarget};
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
  TickResult bus_result = PerformBusAction(mop.bus_action, mop.params, cycle_time);
  if (bus_result.reason != TickStopReason::kReachedTarget) {
    return StepResult{bus_result.reason, true};
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
  return StepResult{TickStopReason::kReachedTarget, false};
}

// Returns 0 when the CPU can't estimate (e.g., mid-fetch); loop still forward-progresses.
TimeMasterDeltaT CPU::EstimateNextStepCostOrZero() const {
  // Peek-only: planning a bus transaction is pure. Go through `snes_` so we
  // work even before CPU::Reset has cached system_bus_raw_ (some tests
  // construct a CPU without resetting it before calling Tick).
  if (snes_ == nullptr || snes_->system_bus == nullptr) {
    return kInternalCpuCycleMaster;
  }
  const SystemBus& bus = *snes_->system_bus;

  if (ShouldFetchInstruction()) {
    return bus.Plan(PcAddr(regs_), BusAccessType::kRead).access_cycles;
  }

  assert(current_instr_ != nullptr);
  const uint8_t op_idx = static_cast<uint8_t>(micro_op_index_ - 1U);
  assert(op_idx < current_instr_->remaining_op_count);
  const MicroOp& mop = current_instr_->ops[op_idx];

  switch (mop.bus_action) {
    case MicroBusAction::kNone: return kInternalCpuCycleMaster;
    case MicroBusAction::kFetchPc: return bus.Plan(PcAddr(regs_), BusAccessType::kRead).access_cycles;
    case MicroBusAction::kReadAddr: return bus.Plan(addr_, BusAccessType::kRead).access_cycles;
    case MicroBusAction::kWriteRegByte: return bus.Plan(addr_, BusAccessType::kWrite).access_cycles;
    case MicroBusAction::kPushStack: return bus.Plan(StackAddr(regs_), BusAccessType::kWrite).access_cycles;
    case MicroBusAction::kPullStack: return bus.Plan(StackAddr(regs_), BusAccessType::kRead).access_cycles;
    case MicroBusAction::kPreIncPullStack: {
      // Simulate the pre-increment on a copy so the estimate picks the
      // page the real read will land on. Mirror the wide-mode logic from
      // PerformBusAction so the page estimate matches the live increment.
      CPU::Regs sim = regs_;
      const bool wide = (current_instr_ != nullptr) && current_instr_->is_new_65816_instruction;
      if (sim.P.E && !wide) {
        const uint8_t sp_lo = static_cast<uint8_t>(static_cast<uint8_t>(sim.SP) + 1U);
        sim.SP = static_cast<uint16_t>(0x0100U | sp_lo);
      } else {
        sim.SP = static_cast<uint16_t>(sim.SP + 1U);
      }
      return bus.Plan(StackAddr(sim), BusAccessType::kRead).access_cycles;
    }
  }
  return kInternalCpuCycleMaster;
}

TickResult CPU::TickToTarget(TimeMasterT target_master_time) {
  if (fault_.has_value()) {
    return {0, TickStopReason::kFault};
  }
  if (snes_ == nullptr) {
    return {0, TickStopReason::kReachedTarget};
  }

  const TimeMasterT start = snes_->GetMasterTime();

  // Bring the DRAM refresh cursor up if a halt (STP/WAI) skipped master time
  // forward since the last main-loop iteration. Real hardware continues to
  // refresh during WAI — the bus stalls but the refresh clock ticks — so
  // post-wake we shouldn't charge CPU time for every missed window. The main
  // loop's refresh trigger stops one scanline past current time.
  auto catch_up_refresh_cursor = [this]() {
    if (next_refresh_time_ > local_time_) return;
    const TimeMasterT behind = local_time_ - next_refresh_time_;
    const TimeMasterT windows = (behind / kMasterCyclesPerScanline) + 1U;
    next_refresh_time_ += windows * kMasterCyclesPerScanline;
  };

  // STP: consume cycles to target, never wake, never sample. Only Reset()
  // clears kStp.
  if (halt_state_ == HaltState::kStp) {
    snes_->SetMasterTime(target_master_time);
    local_time_ = target_master_time;
    return {target_master_time - start, TickStopReason::kReachedTarget};
  }

  // WAI: sample the raw /NMI line at entry. Any pin assertion (NMI/IRQ/ABORT)
  // wakes, regardless of I flag — delivery is gated separately at the next
  // instruction-boundary sample. On wake, we charge a 2-cycle internal latency
  // (per WDC §18 / Bruce Clark §19.1) before transitioning to kNone and
  // falling through to normal dispatch. The latency consumes master time but
  // issues no bus cycles.
  if (halt_state_ == HaltState::kWai) {
    // Trigger wake if a pin is asserted and we haven't started the countdown.
    if (wai_wake_cycles_remaining_ == 0 && WaiShouldWake(snes_->GetMasterTime())) {
      wai_wake_cycles_remaining_ = 2;
    }

    if (wai_wake_cycles_remaining_ > 0) {
      while (wai_wake_cycles_remaining_ > 0 && snes_->GetMasterTime() < target_master_time) {
        const TimeMasterDeltaT step = kInternalCpuCycleMaster;
        if (snes_->GetMasterTime() + step > target_master_time) {
          // Budget exhausted mid-wake cycle — consume what we can and bail;
          // resume on next Tick.
          const TimeMasterDeltaT avail = target_master_time - snes_->GetMasterTime();
          snes_->SetMasterTime(target_master_time);
          local_time_ = target_master_time;
          (void)avail;
          return {snes_->GetMasterTime() - start, TickStopReason::kReachedTarget};
        }
        snes_->SetMasterTime(snes_->GetMasterTime() + step);
        local_time_ = snes_->GetMasterTime();
        --wai_wake_cycles_remaining_;
      }
      if (wai_wake_cycles_remaining_ == 0) {
        halt_state_ = HaltState::kNone;
        catch_up_refresh_cursor();
        // Fall through to the main dispatch loop below.
      } else {
        // Still waking, budget exhausted.
        return {snes_->GetMasterTime() - start, TickStopReason::kReachedTarget};
      }
    } else {
      // No wake yet — consume remaining budget as idle internal cycles.
      snes_->SetMasterTime(target_master_time);
      local_time_ = target_master_time;
      return {target_master_time - start, TickStopReason::kReachedTarget};
    }
  }

  while (snes_->GetMasterTime() < target_master_time) {
    // Instruction-boundary interrupt sampling. Runs once per instruction,
    // before EstimateNextStepCostOrZero sees the next op, so the estimator
    // picks up the synthetic-entry's first micro-op cost (internal cycle)
    // rather than a bus-fetch cost. Hardware: real 65C816 samples NMI/IRQ/
    // ABORT near the end of each instruction and replaces the next opcode
    // fetch with a 7/8-cycle interrupt sequence (WDC §9, Bruce Clark §6.13).
    if (ShouldFetchInstruction()) {
      SampleInterrupts(snes_->GetMasterTime());
      if (const auto kind = SelectPendingInterrupt(); kind.has_value()) {
        if (const InstructionEntry* entry = opcode_defs_internal::HwInterruptEntryFor(*kind); entry != nullptr) {
          // Consume the pending flip-flop (interrupt-acknowledge). NMI's
          // flip-flop stays consumed until the next falling edge; the
          // re-arm logic lives in SampleInterrupts.
          if (*kind == InterruptKind::kNmi) {
            nmi_pending_ = false;
          } else if (*kind == InterruptKind::kAbort) {
            abort_pending_ = false;
          }
          current_instr_ = entry;
          needs_drain_ = entry->rule_count > 1;
          micro_op_index_ = 1;
          timing_context_ = TimingContext{};
          // Trace entry for the interrupt handler invocation. PC points at
          // the instruction that would have executed had the interrupt not
          // been taken — same as where RTI will return to.
          pending_trace_ = TraceEntry{local_time_, PcAddr(regs_), regs_};
          if (micro_op_recorder_ != nullptr) {
            // Use the kind-as-opcode so debuggers can render "NMI" / "IRQ" /
            // "ABORT" via the standard instruction-begin hook. No actual byte
            // was fetched; the bus value is informational.
            micro_op_recorder_->OnInstructionBegin(static_cast<uint8_t>(*kind), PcAddr(regs_));
          }
        }
      }
    }

    // DRAM refresh: stall the CPU for its window, consume mcyc without
    // issuing bus ops. Clamp to remaining target.
    if (refresh_cycles_remaining_ > 0) {
      const TimeMasterDeltaT available = target_master_time - snes_->GetMasterTime();
      const TimeMasterDeltaT take = std::min<TimeMasterDeltaT>(refresh_cycles_remaining_, available);
      snes_->SetMasterTime(snes_->GetMasterTime() + take);
      local_time_ = snes_->GetMasterTime();
      refresh_cycles_remaining_ -= take;
      retired_refresh_cycles_ += take;
      continue;
    }
    if (local_time_ >= next_refresh_time_) {
      refresh_cycles_remaining_ = kDramRefreshDurationCycles;
      next_refresh_time_ += kMasterCyclesPerScanline;
      ++retired_refresh_windows_;
      continue;
    }

    // Partial-op bookkeeping: determine how many cycles the next micro-op
    // still needs after deducting cycles already banked from a prior call.
    const TimeMasterDeltaT cost = EstimateNextStepCostOrZero();
    const TimeMasterDeltaT remaining = (cost > partial_op_cycles_) ? cost - partial_op_cycles_ : 0;

    if (remaining > 0 && snes_->GetMasterTime() + remaining > target_master_time) {
      // Target lands mid-op. Bank the partial progress, don't execute the op.
      // On the next TickToTarget call, remaining cost = cost - partial_op_cycles_.
      const TimeMasterDeltaT avail = target_master_time - snes_->GetMasterTime();
      partial_op_cycles_ += avail;
      snes_->SetMasterTime(target_master_time);
      local_time_ = snes_->GetMasterTime();
      return {snes_->GetMasterTime() - start, TickStopReason::kReachedTarget};
    }

    // Op fits (or estimator returned 0). Advance master_time FIRST to op-end
    // so that bus accesses inside the micro-op see the op-end timestamp —
    // hardware-accurate: bus write/read lands at the end of the bus cycle.
    snes_->SetMasterTime(snes_->GetMasterTime() + remaining);
    local_time_ = snes_->GetMasterTime();
    partial_op_cycles_ = 0;

    // A micro-op that just retired may have set halt_state_ (WAI/STP). Exit the
    // dispatch loop immediately; the next TickToTarget call enters via the
    // halt-state prologue above.
    if (halt_state_ != HaltState::kNone && ShouldFetchInstruction()) {
      snes_->SetMasterTime(target_master_time);
      local_time_ = target_master_time;
      return {snes_->GetMasterTime() - start, TickStopReason::kReachedTarget};
    }
    StepResult step = ShouldFetchInstruction() ? FetchOpcode(0) : ExecuteMicroOp(0);
    if (step.stopped) {
      return {snes_->GetMasterTime() - start,
              step.reason == TickStopReason::kFault ? TickStopReason::kFault : TickStopReason::kBreakpoint};
    }

    const bool at_instruction_boundary = ShouldFetchInstruction();
    const bool microop_mode = debugger_contract_.step_granularity == DebuggerContract::StepGranularity::kMicroOp;
    const bool yield_for_step = (at_instruction_boundary || microop_mode);

    if (yield_for_step && debugger_contract_.step_target > 0) {
      --debugger_contract_.step_target;
      if (debugger_contract_.step_target == 0) {
        return {snes_->GetMasterTime() - start, TickStopReason::kRetiredStepTarget};
      }
    }
  }
  return {snes_->GetMasterTime() - start, TickStopReason::kReachedTarget};
}

}  // namespace pupsnes
