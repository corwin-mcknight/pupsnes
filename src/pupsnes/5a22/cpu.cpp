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

// dest = src but only replace the low byte (or full 16 bits) according to width.
[[gnu::always_inline]] inline uint16_t MergeByWidth(uint16_t dest, uint16_t src, bool wide) {
  return wide ? src : static_cast<uint16_t>((dest & 0xFF00U) | (src & 0x00FFU));
}

[[gnu::always_inline]] inline void SetAddrByteFromFetch(uint32_t& addr, uint8_t fetch, unsigned shift) {
  const uint32_t mask = ~(uint32_t{0xFFU} << shift) & 0xFFFFFFU;
  addr = (addr & mask) | (static_cast<uint32_t>(fetch) << shift);
}

[[gnu::always_inline]] inline uint16_t AluOperand16FromFetch(uint32_t addr, uint8_t fetch) {
  const uint16_t low = static_cast<uint16_t>(addr & 0xFFU);
  const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(fetch) << 8U);
  return static_cast<uint16_t>(low | high);
}

[[gnu::always_inline]] inline void LoadA8UpdateNz(CpuRegs& regs, uint8_t fetch) {
  regs.A = static_cast<uint16_t>((regs.A & 0xFF00U) | fetch);
  regs.P.Z = (static_cast<uint8_t>(regs.A) == 0U);
  regs.P.N = (regs.A & 0x0080U) != 0U;
}
[[gnu::always_inline]] inline void LoadALow(CpuRegs& regs, uint8_t fetch) {
  regs.A = static_cast<uint16_t>((regs.A & 0xFF00U) | fetch);
}
[[gnu::always_inline]] inline void LoadAHighUpdateNz(CpuRegs& regs, uint8_t fetch) {
  const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(fetch) << 8U);
  regs.A = static_cast<uint16_t>(high | (regs.A & 0x00FFU));
  regs.P.Z = (regs.A == 0U);
  regs.P.N = (regs.A & 0x8000U) != 0U;
}
[[gnu::always_inline]] inline void LoadX8UpdateNz(CpuRegs& regs, uint8_t fetch) {
  regs.X = static_cast<uint16_t>((regs.X & 0xFF00U) | fetch);
  regs.P.Z = (static_cast<uint8_t>(regs.X) == 0U);
  regs.P.N = (regs.X & 0x0080U) != 0U;
}
[[gnu::always_inline]] inline void LoadXLow(CpuRegs& regs, uint8_t fetch) {
  regs.X = static_cast<uint16_t>((regs.X & 0xFF00U) | fetch);
}
[[gnu::always_inline]] inline void LoadXHighUpdateNz(CpuRegs& regs, uint8_t fetch) {
  const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(fetch) << 8U);
  regs.X = static_cast<uint16_t>(high | (regs.X & 0x00FFU));
  regs.P.Z = (regs.X == 0U);
  regs.P.N = (regs.X & 0x8000U) != 0U;
}
[[gnu::always_inline]] inline void LoadY8UpdateNz(CpuRegs& regs, uint8_t fetch) {
  regs.Y = static_cast<uint16_t>((regs.Y & 0xFF00U) | fetch);
  regs.P.Z = (static_cast<uint8_t>(regs.Y) == 0U);
  regs.P.N = (regs.Y & 0x0080U) != 0U;
}
[[gnu::always_inline]] inline void LoadYLow(CpuRegs& regs, uint8_t fetch) {
  regs.Y = static_cast<uint16_t>((regs.Y & 0xFF00U) | fetch);
}
[[gnu::always_inline]] inline void LoadYHighUpdateNz(CpuRegs& regs, uint8_t fetch) {
  const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(fetch) << 8U);
  regs.Y = static_cast<uint16_t>(high | (regs.Y & 0x00FFU));
  regs.P.Z = (regs.Y == 0U);
  regs.P.N = (regs.Y & 0x8000U) != 0U;
}

[[gnu::always_inline]] inline void BranchRelative8(CpuRegs& regs, uint8_t fetch, bool& branch_page_crossed) {
  const int8_t displacement = static_cast<int8_t>(fetch);
  const uint16_t old_pc = regs.PC;
  regs.PC = static_cast<uint16_t>(regs.PC + displacement);
  branch_page_crossed = ((old_pc ^ regs.PC) & 0xFF00U) != 0U;
}

[[gnu::always_inline]] inline void DecrementSp(CpuRegs& regs) {
  if (regs.P.E) {
    const uint8_t sp_lo = static_cast<uint8_t>(static_cast<uint8_t>(regs.SP) - 1U);
    regs.SP = static_cast<uint16_t>(0x0100U | sp_lo);
  } else {
    regs.SP = static_cast<uint16_t>(regs.SP - 1U);
  }
}

[[gnu::always_inline]] inline void IncrementSp(CpuRegs& regs) {
  if (regs.P.E) {
    const uint8_t sp_lo = static_cast<uint8_t>(static_cast<uint8_t>(regs.SP) + 1U);
    regs.SP = static_cast<uint16_t>(0x0100U | sp_lo);
  } else {
    regs.SP = static_cast<uint16_t>(regs.SP + 1U);
  }
}

[[gnu::always_inline]] inline void LoadDbrUpdateNz(CpuRegs& regs, uint8_t fetch) {
  regs.DBR = fetch;
  regs.P.Z = (regs.DBR == 0U);
  regs.P.N = (regs.DBR & 0x80U) != 0U;
}

[[gnu::always_inline]] inline void IncA(CpuRegs& regs) {
  if (IsAccumulator16Bit(regs)) {
    regs.A = static_cast<uint16_t>(regs.A + 1U);
    regs.P.Z = (regs.A == 0U);
    regs.P.N = (regs.A & 0x8000U) != 0U;
  } else {
    const uint8_t lo = static_cast<uint8_t>(static_cast<uint8_t>(regs.A) + 1U);
    regs.A = static_cast<uint16_t>((regs.A & 0xFF00U) | lo);
    regs.P.Z = (lo == 0U);
    regs.P.N = (lo & 0x80U) != 0U;
  }
}
[[gnu::always_inline]] inline void DecA(CpuRegs& regs) {
  if (IsAccumulator16Bit(regs)) {
    regs.A = static_cast<uint16_t>(regs.A - 1U);
    regs.P.Z = (regs.A == 0U);
    regs.P.N = (regs.A & 0x8000U) != 0U;
  } else {
    const uint8_t lo = static_cast<uint8_t>(static_cast<uint8_t>(regs.A) - 1U);
    regs.A = static_cast<uint16_t>((regs.A & 0xFF00U) | lo);
    regs.P.Z = (lo == 0U);
    regs.P.N = (lo & 0x80U) != 0U;
  }
}
[[gnu::always_inline]] inline void IncX(CpuRegs& regs) {
  if (IsIndex16Bit(regs)) {
    regs.X = static_cast<uint16_t>(regs.X + 1U);
    regs.P.Z = (regs.X == 0U);
    regs.P.N = (regs.X & 0x8000U) != 0U;
  } else {
    const uint8_t lo = static_cast<uint8_t>(static_cast<uint8_t>(regs.X) + 1U);
    regs.X = static_cast<uint16_t>((regs.X & 0xFF00U) | lo);
    regs.P.Z = (lo == 0U);
    regs.P.N = (lo & 0x80U) != 0U;
  }
}
[[gnu::always_inline]] inline void DecX(CpuRegs& regs) {
  if (IsIndex16Bit(regs)) {
    regs.X = static_cast<uint16_t>(regs.X - 1U);
    regs.P.Z = (regs.X == 0U);
    regs.P.N = (regs.X & 0x8000U) != 0U;
  } else {
    const uint8_t lo = static_cast<uint8_t>(static_cast<uint8_t>(regs.X) - 1U);
    regs.X = static_cast<uint16_t>((regs.X & 0xFF00U) | lo);
    regs.P.Z = (lo == 0U);
    regs.P.N = (lo & 0x80U) != 0U;
  }
}
[[gnu::always_inline]] inline void IncY(CpuRegs& regs) {
  if (IsIndex16Bit(regs)) {
    regs.Y = static_cast<uint16_t>(regs.Y + 1U);
    regs.P.Z = (regs.Y == 0U);
    regs.P.N = (regs.Y & 0x8000U) != 0U;
  } else {
    const uint8_t lo = static_cast<uint8_t>(static_cast<uint8_t>(regs.Y) + 1U);
    regs.Y = static_cast<uint16_t>((regs.Y & 0xFF00U) | lo);
    regs.P.Z = (lo == 0U);
    regs.P.N = (lo & 0x80U) != 0U;
  }
}
[[gnu::always_inline]] inline void DecY(CpuRegs& regs) {
  if (IsIndex16Bit(regs)) {
    regs.Y = static_cast<uint16_t>(regs.Y - 1U);
    regs.P.Z = (regs.Y == 0U);
    regs.P.N = (regs.Y & 0x8000U) != 0U;
  } else {
    const uint8_t lo = static_cast<uint8_t>(static_cast<uint8_t>(regs.Y) - 1U);
    regs.Y = static_cast<uint16_t>((regs.Y & 0xFF00U) | lo);
    regs.P.Z = (lo == 0U);
    regs.P.N = (lo & 0x80U) != 0U;
  }
}

// Dispatch a parameterized register inc/dec to the concrete per-register
// helper above. The switch collapses at compile time when (reg, decrement) are
// compile-time constants (true for all current MakeIncDecSpecs entries), so
// the hot path remains a direct call — no runtime table lookup.
[[gnu::always_inline]] inline void DispatchIncDecReg(CpuRegs& regs, Reg reg, bool decrement) {
  switch (reg) {
    case Reg::kA:
      if (decrement) {
        DecA(regs);
      } else {
        IncA(regs);
      }
      return;
    case Reg::kX:
      if (decrement) {
        DecX(regs);
      } else {
        IncX(regs);
      }
      return;
    case Reg::kY:
      if (decrement) {
        DecY(regs);
      } else {
        IncY(regs);
      }
      return;
    default:
      break;
  }
  __builtin_unreachable();
}

// Dispatch a parameterized flag set/clear to the concrete P-register field.
// The switch collapses at compile time when (flag, value) are compile-time
// constants (true for every MakeFlagSpecs entry that emits kSetFlag), so the
// hot path is a single store — no runtime lookup. Only C/D/I/V are reachable
// via real opcodes; any other Flag value is a builder bug.
[[gnu::always_inline]] inline void DispatchSetFlag(CpuRegs& regs, Flag flag, bool value) {
  switch (flag) {
    case Flag::kC:
      regs.P.C = value;
      return;
    case Flag::kD:
      regs.P.D = value;
      return;
    case Flag::kI:
      regs.P.I = value;
      return;
    case Flag::kV:
      regs.P.V = value;
      return;
    default:
      break;
  }
  __builtin_unreachable();
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

[[gnu::always_inline]] inline void RepFromFetch(CpuRegs& regs, uint8_t fetch) {
  uint8_t p = regs.P.ToByte();
  p = static_cast<uint8_t>(p & ~fetch);
  regs.P.FromByte(p, regs.P.E);
  ApplyEmulationForcing(regs);
}
[[gnu::always_inline]] inline void SepFromFetch(CpuRegs& regs, uint8_t fetch) {
  uint8_t p = regs.P.ToByte();
  p = static_cast<uint8_t>(p | fetch);
  regs.P.FromByte(p, regs.P.E);
  ApplyEmulationForcing(regs);
}
[[gnu::always_inline]] inline void ExchangeCarryEmulation(CpuRegs& regs) {
  const bool new_e = regs.P.C;
  const bool new_c = regs.P.E;
  regs.P.E = new_e;
  regs.P.C = new_c;
  ApplyEmulationForcing(regs);
}

[[gnu::always_inline]] inline void TransferAToX(CpuRegs& regs) {
  const bool wide = IsIndex16Bit(regs);
  regs.X = MergeByWidth(regs.X, regs.A, wide);
  SetNzFromWidth(regs, regs.X, wide);
}
[[gnu::always_inline]] inline void TransferAToY(CpuRegs& regs) {
  const bool wide = IsIndex16Bit(regs);
  regs.Y = MergeByWidth(regs.Y, regs.A, wide);
  SetNzFromWidth(regs, regs.Y, wide);
}
[[gnu::always_inline]] inline void TransferSToX(CpuRegs& regs) {
  const bool wide = IsIndex16Bit(regs);
  regs.X = MergeByWidth(regs.X, regs.SP, wide);
  SetNzFromWidth(regs, regs.X, wide);
}
[[gnu::always_inline]] inline void TransferXToA(CpuRegs& regs) {
  const bool wide = IsAccumulator16Bit(regs);
  regs.A = MergeByWidth(regs.A, regs.X, wide);
  SetNzFromWidth(regs, regs.A, wide);
}
[[gnu::always_inline]] inline void TransferXToS(CpuRegs& regs) {
  // SP is always written full width except that E=1 forces SH back to $01.
  regs.SP = regs.X;
  if (regs.P.E) {
    regs.SP = static_cast<uint16_t>(0x0100U | (regs.SP & 0x00FFU));
  }
}
[[gnu::always_inline]] inline void TransferXToY(CpuRegs& regs) {
  const bool wide = IsIndex16Bit(regs);
  regs.Y = MergeByWidth(regs.Y, regs.X, wide);
  SetNzFromWidth(regs, regs.Y, wide);
}
[[gnu::always_inline]] inline void TransferYToA(CpuRegs& regs) {
  const bool wide = IsAccumulator16Bit(regs);
  regs.A = MergeByWidth(regs.A, regs.Y, wide);
  SetNzFromWidth(regs, regs.A, wide);
}
[[gnu::always_inline]] inline void TransferYToX(CpuRegs& regs) {
  const bool wide = IsIndex16Bit(regs);
  regs.X = MergeByWidth(regs.X, regs.Y, wide);
  SetNzFromWidth(regs, regs.X, wide);
}
[[gnu::always_inline]] inline void TransferAToD(CpuRegs& regs) {
  regs.DP = regs.A;
  SetNzFromWidth(regs, regs.DP, true);
}
[[gnu::always_inline]] inline void TransferAToS(CpuRegs& regs) {
  regs.SP = regs.A;
  if (regs.P.E) {
    regs.SP = static_cast<uint16_t>(0x0100U | (regs.SP & 0x00FFU));
  }
}
[[gnu::always_inline]] inline void TransferDToA(CpuRegs& regs) {
  regs.A = regs.DP;
  SetNzFromWidth(regs, regs.A, true);
}
[[gnu::always_inline]] inline void TransferSToA(CpuRegs& regs) {
  regs.A = regs.SP;
  SetNzFromWidth(regs, regs.A, true);
}

// Dispatch a parameterized register-to-register transfer to the concrete
// per-pair helper above. The switch collapses at compile time when the (src,
// dst) pair is a known constant (true for all current MakeTransferSpecs
// entries), so the hot path remains a direct call — no runtime table lookup.
[[gnu::always_inline]] inline void DispatchTransferReg(CpuRegs& regs, Reg src, Reg dst) {
  switch (src) {
    case Reg::kA:
      switch (dst) {
        case Reg::kX:
          TransferAToX(regs);
          return;
        case Reg::kY:
          TransferAToY(regs);
          return;
        case Reg::kSp:
          TransferAToS(regs);
          return;
        case Reg::kDp:
          TransferAToD(regs);
          return;
        default:
          break;
      }
      break;
    case Reg::kX:
      switch (dst) {
        case Reg::kA:
          TransferXToA(regs);
          return;
        case Reg::kY:
          TransferXToY(regs);
          return;
        case Reg::kSp:
          TransferXToS(regs);
          return;
        default:
          break;
      }
      break;
    case Reg::kY:
      switch (dst) {
        case Reg::kA:
          TransferYToA(regs);
          return;
        case Reg::kX:
          TransferYToX(regs);
          return;
        default:
          break;
      }
      break;
    case Reg::kSp:
      switch (dst) {
        case Reg::kA:
          TransferSToA(regs);
          return;
        case Reg::kX:
          TransferSToX(regs);
          return;
        default:
          break;
      }
      break;
    case Reg::kDp:
      if (dst == Reg::kA) {
        TransferDToA(regs);
        return;
      }
      break;
    default:
      break;
  }
  __builtin_unreachable();
}

[[gnu::always_inline]] inline void BranchRelative16(CpuRegs& regs, uint32_t addr) {
  const int16_t displacement = static_cast<int16_t>(static_cast<uint16_t>(addr & 0xFFFFU));
  regs.PC = static_cast<uint16_t>(regs.PC + displacement);
}
[[gnu::always_inline]] inline void SetPcFromAddr(CpuRegs& regs, uint32_t addr) {
  regs.PC = static_cast<uint16_t>(addr & 0xFFFFU);
}
[[gnu::always_inline]] inline void SetPcAndPbrFromAddr(CpuRegs& regs, uint32_t addr) {
  regs.PC = static_cast<uint16_t>(addr & 0xFFFFU);
  regs.PBR = static_cast<uint8_t>((addr >> 16U) & 0xFFU);
}

// Binary ADC helper. BCD/decimal mode is not yet implemented; in D=1 the
// behavior falls back to binary arithmetic (and v flag is overwritten).
[[gnu::always_inline]] inline void AluAdc8(CpuRegs& regs, uint8_t operand) {
  const uint16_t a_lo = static_cast<uint8_t>(regs.A);
  const uint16_t sum = static_cast<uint16_t>(a_lo + operand + (regs.P.C ? 1U : 0U));
  const uint8_t result = static_cast<uint8_t>(sum);
  const bool overflow = ((~(a_lo ^ operand) & (a_lo ^ result)) & 0x80U) != 0U;
  regs.A = static_cast<uint16_t>((regs.A & 0xFF00U) | result);
  regs.P.C = (sum & 0x0100U) != 0U;
  regs.P.V = overflow;
  regs.P.Z = (result == 0U);
  regs.P.N = (result & 0x80U) != 0U;
}
[[gnu::always_inline]] inline void AluAdc16(CpuRegs& regs, uint16_t operand) {
  const uint32_t a = regs.A;
  const uint32_t sum = a + operand + (regs.P.C ? 1U : 0U);
  const uint16_t result = static_cast<uint16_t>(sum);
  const bool overflow = ((~(a ^ operand) & (a ^ result)) & 0x8000U) != 0U;
  regs.A = result;
  regs.P.C = (sum & 0x10000U) != 0U;
  regs.P.V = overflow;
  regs.P.Z = (result == 0U);
  regs.P.N = (result & 0x8000U) != 0U;
}
// SBC binary: A - operand - (1 - C). Implement as ADC of ~operand.
[[gnu::always_inline]] inline void AluSbc8(CpuRegs& regs, uint8_t operand) {
  AluAdc8(regs, static_cast<uint8_t>(~operand));
}
[[gnu::always_inline]] inline void AluSbc16(CpuRegs& regs, uint16_t operand) {
  AluAdc16(regs, static_cast<uint16_t>(~operand));
}
[[gnu::always_inline]] inline void AluAnd8(CpuRegs& regs, uint8_t operand) {
  const uint8_t result = static_cast<uint8_t>(regs.A) & operand;
  regs.A = static_cast<uint16_t>((regs.A & 0xFF00U) | result);
  regs.P.Z = (result == 0U);
  regs.P.N = (result & 0x80U) != 0U;
}
[[gnu::always_inline]] inline void AluAnd16(CpuRegs& regs, uint16_t operand) {
  regs.A = static_cast<uint16_t>(regs.A & operand);
  regs.P.Z = (regs.A == 0U);
  regs.P.N = (regs.A & 0x8000U) != 0U;
}
[[gnu::always_inline]] inline void AluOra8(CpuRegs& regs, uint8_t operand) {
  const uint8_t result = static_cast<uint8_t>(regs.A) | operand;
  regs.A = static_cast<uint16_t>((regs.A & 0xFF00U) | result);
  regs.P.Z = (result == 0U);
  regs.P.N = (result & 0x80U) != 0U;
}
[[gnu::always_inline]] inline void AluOra16(CpuRegs& regs, uint16_t operand) {
  regs.A = static_cast<uint16_t>(regs.A | operand);
  regs.P.Z = (regs.A == 0U);
  regs.P.N = (regs.A & 0x8000U) != 0U;
}
[[gnu::always_inline]] inline void AluEor8(CpuRegs& regs, uint8_t operand) {
  const uint8_t result = static_cast<uint8_t>(regs.A) ^ operand;
  regs.A = static_cast<uint16_t>((regs.A & 0xFF00U) | result);
  regs.P.Z = (result == 0U);
  regs.P.N = (result & 0x80U) != 0U;
}
[[gnu::always_inline]] inline void AluEor16(CpuRegs& regs, uint16_t operand) {
  regs.A = static_cast<uint16_t>(regs.A ^ operand);
  regs.P.Z = (regs.A == 0U);
  regs.P.N = (regs.A & 0x8000U) != 0U;
}
[[gnu::always_inline]] inline void DoCompare8(CpuRegs& regs, uint8_t reg, uint8_t operand) {
  const uint16_t diff = static_cast<uint16_t>(reg) - static_cast<uint16_t>(operand);
  regs.P.C = reg >= operand;
  regs.P.Z = (static_cast<uint8_t>(diff) == 0U);
  regs.P.N = (diff & 0x80U) != 0U;
}
[[gnu::always_inline]] inline void DoCompare16(CpuRegs& regs, uint16_t reg, uint16_t operand) {
  const uint32_t diff = static_cast<uint32_t>(reg) - static_cast<uint32_t>(operand);
  regs.P.C = reg >= operand;
  regs.P.Z = (static_cast<uint16_t>(diff) == 0U);
  regs.P.N = (diff & 0x8000U) != 0U;
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
  switch (op) {
    case MicroInternalOp::kNone:
      break;
    case MicroInternalOp::kLoadA8UpdateNz:
      LoadA8UpdateNz(regs_, fetch_data_);
      break;
    case MicroInternalOp::kLoadALow:
      LoadALow(regs_, fetch_data_);
      break;
    case MicroInternalOp::kLoadAHighUpdateNz:
      LoadAHighUpdateNz(regs_, fetch_data_);
      break;
    case MicroInternalOp::kLoadX8UpdateNz:
      LoadX8UpdateNz(regs_, fetch_data_);
      break;
    case MicroInternalOp::kLoadXLow:
      LoadXLow(regs_, fetch_data_);
      break;
    case MicroInternalOp::kLoadXHighUpdateNz:
      LoadXHighUpdateNz(regs_, fetch_data_);
      break;
    case MicroInternalOp::kLoadY8UpdateNz:
      LoadY8UpdateNz(regs_, fetch_data_);
      break;
    case MicroInternalOp::kLoadYLow:
      LoadYLow(regs_, fetch_data_);
      break;
    case MicroInternalOp::kLoadYHighUpdateNz:
      LoadYHighUpdateNz(regs_, fetch_data_);
      break;
    case MicroInternalOp::kSetBranchTaken:
      timing_context_.branch_taken = true;
      break;
    case MicroInternalOp::kSetBranchTakenIfNotZero:
      timing_context_.branch_taken = !regs_.P.Z;
      break;
    case MicroInternalOp::kBranchRelative8:
      BranchRelative8(regs_, fetch_data_, timing_context_.branch_page_crossed);
      break;
    case MicroInternalOp::kSetAddrLowFromFetch:
      SetAddrByteFromFetch(addr_, fetch_data_, 0);
      break;
    case MicroInternalOp::kSetAddrHighFromFetch:
      SetAddrByteFromFetch(addr_, fetch_data_, 8);
      break;
    case MicroInternalOp::kSetAddrBankFromFetch:
      SetAddrByteFromFetch(addr_, fetch_data_, 16);
      break;
    case MicroInternalOp::kSetAddrHighFromFetchAndBankFromDbr:
      SetAddrByteFromFetch(addr_, fetch_data_, 8);
      addr_ = (addr_ & 0x00FFFFU) | (static_cast<uint32_t>(regs_.DBR) << 16U);
      break;
    case MicroInternalOp::kIncrementAddr:
      addr_ = (addr_ + 1U) & 0xFFFFFFU;
      break;
    case MicroInternalOp::kDecrementSp:
      DecrementSp(regs_);
      break;
    case MicroInternalOp::kIncrementSp:
      IncrementSp(regs_);
      break;
    case MicroInternalOp::kLoadDbrUpdateNz:
      LoadDbrUpdateNz(regs_, fetch_data_);
      break;
    case MicroInternalOp::kIncDecReg:
      DispatchIncDecReg(regs_, opcode_defs_internal::micro_op_params::UnpackIncDecReg(params),
                        opcode_defs_internal::micro_op_params::UnpackIncDecDecrement(params));
      break;
    case MicroInternalOp::kSetFlag:
      DispatchSetFlag(regs_, opcode_defs_internal::micro_op_params::UnpackSetFlagFlag(params),
                      opcode_defs_internal::micro_op_params::UnpackSetFlagValue(params));
      break;
    case MicroInternalOp::kRepFromFetch:
      RepFromFetch(regs_, fetch_data_);
      break;
    case MicroInternalOp::kSepFromFetch:
      SepFromFetch(regs_, fetch_data_);
      break;
    case MicroInternalOp::kExchangeCarryEmulation:
      ExchangeCarryEmulation(regs_);
      break;
    case MicroInternalOp::kTransferReg:
      DispatchTransferReg(regs_, opcode_defs_internal::micro_op_params::UnpackTransferSrc(params),
                          opcode_defs_internal::micro_op_params::UnpackTransferDst(params));
      break;
    case MicroInternalOp::kSetBranchTakenIfZero:
      timing_context_.branch_taken = regs_.P.Z;
      break;
    case MicroInternalOp::kSetBranchTakenIfCarry:
      timing_context_.branch_taken = regs_.P.C;
      break;
    case MicroInternalOp::kSetBranchTakenIfNotCarry:
      timing_context_.branch_taken = !regs_.P.C;
      break;
    case MicroInternalOp::kSetBranchTakenIfNegative:
      timing_context_.branch_taken = regs_.P.N;
      break;
    case MicroInternalOp::kSetBranchTakenIfNotNegative:
      timing_context_.branch_taken = !regs_.P.N;
      break;
    case MicroInternalOp::kSetBranchTakenIfOverflow:
      timing_context_.branch_taken = regs_.P.V;
      break;
    case MicroInternalOp::kSetBranchTakenIfNotOverflow:
      timing_context_.branch_taken = !regs_.P.V;
      break;
    case MicroInternalOp::kBranchRelative16:
      BranchRelative16(regs_, addr_);
      break;
    case MicroInternalOp::kSetPcFromAddr:
      SetPcFromAddr(regs_, addr_);
      break;
    case MicroInternalOp::kSetPcAndPbrFromAddr:
      SetPcAndPbrFromAddr(regs_, addr_);
      break;
    case MicroInternalOp::kSetAddrHighFromFetchAndSetPc:
      SetAddrByteFromFetch(addr_, fetch_data_, 8);
      SetPcFromAddr(regs_, addr_);
      break;
    case MicroInternalOp::kSetAddrBankFromFetchAndSetPcAndPbr:
      SetAddrByteFromFetch(addr_, fetch_data_, 16);
      SetPcAndPbrFromAddr(regs_, addr_);
      break;
    case MicroInternalOp::kDecrementPc:
      regs_.PC = static_cast<uint16_t>(regs_.PC - 1U);
      break;
    case MicroInternalOp::kIncrementPc:
      regs_.PC = static_cast<uint16_t>(regs_.PC + 1U);
      break;
    case MicroInternalOp::kSetPclFromFetch:
      regs_.PC = static_cast<uint16_t>((regs_.PC & 0xFF00U) | fetch_data_);
      break;
    case MicroInternalOp::kSetPchFromFetch: {
      const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(fetch_data_) << 8U);
      regs_.PC = static_cast<uint16_t>((regs_.PC & 0x00FFU) | high);
      break;
    }
    case MicroInternalOp::kSetPbrFromFetch:
      regs_.PBR = fetch_data_;
      break;
    case MicroInternalOp::kLoadPFromFetch:
      regs_.P.FromByte(fetch_data_, regs_.P.E);
      break;
    case MicroInternalOp::kLoadDpLowFromFetch:
      regs_.DP = static_cast<uint16_t>((regs_.DP & 0xFF00U) | fetch_data_);
      break;
    case MicroInternalOp::kLoadDpHighFromFetchUpdateNz: {
      const uint16_t high = static_cast<uint16_t>(static_cast<uint16_t>(fetch_data_) << 8U);
      regs_.DP = static_cast<uint16_t>(high | (regs_.DP & 0x00FFU));
      regs_.P.Z = (regs_.DP == 0U);
      regs_.P.N = (regs_.DP & 0x8000U) != 0U;
      break;
    }
    case MicroInternalOp::kLoadXHighFromFetchUpdateNz:
      LoadXHighUpdateNz(regs_, fetch_data_);
      break;
    case MicroInternalOp::kLoadYHighFromFetchUpdateNz:
      LoadYHighUpdateNz(regs_, fetch_data_);
      break;
    case MicroInternalOp::kAluAdc8FromFetch:
      AluAdc8(regs_, fetch_data_);
      break;
    case MicroInternalOp::kAluSbc8FromFetch:
      AluSbc8(regs_, fetch_data_);
      break;
    case MicroInternalOp::kAluAnd8FromFetch:
      AluAnd8(regs_, fetch_data_);
      break;
    case MicroInternalOp::kAluOra8FromFetch:
      AluOra8(regs_, fetch_data_);
      break;
    case MicroInternalOp::kAluEor8FromFetch:
      AluEor8(regs_, fetch_data_);
      break;
    case MicroInternalOp::kAluCmp8FromFetch:
      DoCompare8(regs_, static_cast<uint8_t>(regs_.A), fetch_data_);
      break;
    case MicroInternalOp::kAluCpx8FromFetch:
      DoCompare8(regs_, static_cast<uint8_t>(regs_.X), fetch_data_);
      break;
    case MicroInternalOp::kAluCpy8FromFetch:
      DoCompare8(regs_, static_cast<uint8_t>(regs_.Y), fetch_data_);
      break;
    case MicroInternalOp::kAluBit8ImmFromFetch: {
      const uint8_t result = static_cast<uint8_t>(regs_.A) & fetch_data_;
      regs_.P.Z = (result == 0U);
      break;
    }
    case MicroInternalOp::kAluAdc16FromFetch:
      AluAdc16(regs_, AluOperand16FromFetch(addr_, fetch_data_));
      break;
    case MicroInternalOp::kAluSbc16FromFetch:
      AluSbc16(regs_, AluOperand16FromFetch(addr_, fetch_data_));
      break;
    case MicroInternalOp::kAluAnd16FromFetch:
      AluAnd16(regs_, AluOperand16FromFetch(addr_, fetch_data_));
      break;
    case MicroInternalOp::kAluOra16FromFetch:
      AluOra16(regs_, AluOperand16FromFetch(addr_, fetch_data_));
      break;
    case MicroInternalOp::kAluEor16FromFetch:
      AluEor16(regs_, AluOperand16FromFetch(addr_, fetch_data_));
      break;
    case MicroInternalOp::kAluCmp16FromFetch:
      DoCompare16(regs_, regs_.A, AluOperand16FromFetch(addr_, fetch_data_));
      break;
    case MicroInternalOp::kAluCpx16FromFetch:
      DoCompare16(regs_, regs_.X, AluOperand16FromFetch(addr_, fetch_data_));
      break;
    case MicroInternalOp::kAluCpy16FromFetch:
      DoCompare16(regs_, regs_.Y, AluOperand16FromFetch(addr_, fetch_data_));
      break;
    case MicroInternalOp::kAluBit16ImmFromFetch: {
      const uint16_t result = static_cast<uint16_t>(regs_.A & AluOperand16FromFetch(addr_, fetch_data_));
      regs_.P.Z = (result == 0U);
      break;
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
    case MicroBusAction::kWriteAddr:
      return BusWrite(addr_, fetch_data_, cycle_time);
    case MicroBusAction::kWriteA8Addr:
      return BusWrite(addr_, static_cast<uint8_t>(regs_.A), cycle_time);
    case MicroBusAction::kWriteX8Addr:
      return BusWrite(addr_, static_cast<uint8_t>(regs_.X), cycle_time);
    case MicroBusAction::kWriteY8Addr:
      return BusWrite(addr_, static_cast<uint8_t>(regs_.Y), cycle_time);
    case MicroBusAction::kWriteAHighAddr:
      return BusWrite(addr_, static_cast<uint8_t>(regs_.A >> 8U), cycle_time);
    case MicroBusAction::kWriteXHighAddr:
      return BusWrite(addr_, static_cast<uint8_t>(regs_.X >> 8U), cycle_time);
    case MicroBusAction::kWriteYHighAddr:
      return BusWrite(addr_, static_cast<uint8_t>(regs_.Y >> 8U), cycle_time);
    case MicroBusAction::kPushA8:
      return BusWrite(StackAddr(regs_), static_cast<uint8_t>(regs_.A), cycle_time);
    case MicroBusAction::kPushAHigh:
      return BusWrite(StackAddr(regs_), static_cast<uint8_t>(regs_.A >> 8U), cycle_time);
    case MicroBusAction::kPushDbr:
      return BusWrite(StackAddr(regs_), regs_.DBR, cycle_time);
    case MicroBusAction::kPushPch:
      return BusWrite(StackAddr(regs_), static_cast<uint8_t>(regs_.PC >> 8U), cycle_time);
    case MicroBusAction::kPushPcl:
      return BusWrite(StackAddr(regs_), static_cast<uint8_t>(regs_.PC), cycle_time);
    case MicroBusAction::kPushPbr:
      return BusWrite(StackAddr(regs_), regs_.PBR, cycle_time);
    case MicroBusAction::kPushP:
      return BusWrite(StackAddr(regs_), regs_.P.ToByte(), cycle_time);
    case MicroBusAction::kPushX8:
      return BusWrite(StackAddr(regs_), static_cast<uint8_t>(regs_.X), cycle_time);
    case MicroBusAction::kPushXHigh:
      return BusWrite(StackAddr(regs_), static_cast<uint8_t>(regs_.X >> 8U), cycle_time);
    case MicroBusAction::kPushY8:
      return BusWrite(StackAddr(regs_), static_cast<uint8_t>(regs_.Y), cycle_time);
    case MicroBusAction::kPushYHigh:
      return BusWrite(StackAddr(regs_), static_cast<uint8_t>(regs_.Y >> 8U), cycle_time);
    case MicroBusAction::kPushDpLow:
      return BusWrite(StackAddr(regs_), static_cast<uint8_t>(regs_.DP), cycle_time);
    case MicroBusAction::kPushDpHigh:
      return BusWrite(StackAddr(regs_), static_cast<uint8_t>(regs_.DP >> 8U), cycle_time);
    case MicroBusAction::kPushAddrLow:
      return BusWrite(StackAddr(regs_), static_cast<uint8_t>(addr_ & 0xFFU), cycle_time);
    case MicroBusAction::kPushAddrHigh:
      return BusWrite(StackAddr(regs_), static_cast<uint8_t>((addr_ >> 8U) & 0xFFU), cycle_time);
    case MicroBusAction::kPullStack:
      return BusRead(StackAddr(regs_), cycle_time);
    case MicroBusAction::kPreIncPullStack:
      // Stack pulls: the SP must point at the top of the stack before reading.
      // Increment first, then read.
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
