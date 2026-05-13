#pragma once

// BCD (decimal mode) helpers — D-08: no public API. Used by the kAdc/kSbc
// handler in ExecuteInternalOp when regs_.P.D is set. Algorithm follows the
// canonical 65C816 per-nibble fixup model documented by Bruce Clark
// (docs/external/6502opcodes.md §6.1.1.1) and verified against bsnes/higan's
// WDC65816 ADC/SBC implementation.
//
// Why per-nibble (not a single binary-sum + post-adjust):
//   The V flag in BCD mode is computed on the *partially adjusted* result —
//   after the lower nibble fixups have been applied and any carries propagated
//   into the top nibble, but BEFORE the top-nibble fixup (the +$60 / +$6000
//   step that wraps a >$99 / >$9999 result back into BCD range). Computing V
//   from the raw pre-fixup binary sum is wrong: it disagrees with real
//   hardware whenever a lower-nibble carry flips the bit-7/bit-15 sign of the
//   accumulated result before the top fixup undoes it. cputest-basic test
//   0404 (`ADC #$4470` with A=$3550, D=1) is the canonical 16-bit witness:
//   binary sum is $79C0 (V=0) but the partially-adjusted result is $8020
//   (V=1, the value real hardware reports).
//
// SBC uses the same per-nibble shape with subtractive fixup (subtract 6 from
// any nibble that did NOT generate a carry, subtract $60 from any byte that
// did NOT carry, etc.). This matches the 65C816 datasheet identity SBC = ADC
// of one's-complemented operand. The earlier `BcdAdd(A, ~B, C)` shortcut
// previously rejected here would still be wrong for the same reason: the
// shortcut is a binary-mode identity that does not commute with BCD digit
// fixup.
//
// Header-only inline definitions: BCD is a cold path (entered only when
// regs_.P.D is set) but [[gnu::always_inline]] is preserved so the call
// sites in cpu.cpp keep their existing codegen shape.

#include <cstdint>

namespace pupsnes {

struct BcdResult {
  uint16_t value;  // adjusted result (8-bit path uses low 8 bits only)
  bool carry;      // C flag out
  bool zero;       // Z flag
  bool negative;   // N flag (bit 7 for 8-bit, bit 15 for 16-bit)
  bool overflow;   // V flag — computed on partially-adjusted result
};

[[gnu::always_inline]] inline BcdResult BcdAdd8(uint8_t a, uint8_t b, bool c_in) {
  // Low nibble + add-6 fixup.
  uint32_t result = (a & 0x0FU) + (b & 0x0FU) + (c_in ? 1U : 0U);
  if (result > 0x09U) result += 0x06U;
  bool c = result > 0x0FU;
  // High nibble assembled with low-nibble's carry. V is taken HERE — after
  // the low-fixup carry has propagated into bit 7, before the byte-level
  // fixup that wraps >$99 back into range.
  result = (a & 0xF0U) + (b & 0xF0U) + (c ? 0x10U : 0U) + (result & 0x0FU);
  const bool v_out = ((~(a ^ b) & (a ^ static_cast<uint8_t>(result))) & 0x80U) != 0U;
  if (result > 0x9FU) result += 0x60U;
  const bool c_out = result > 0xFFU;
  const uint8_t final_result = static_cast<uint8_t>(result & 0xFFU);
  return BcdResult{final_result, c_out, final_result == 0U, (final_result & 0x80U) != 0U, v_out};
}

// BCD subtraction 8-bit. Uses the standard 65C816 identity (a + ~b + c_in)
// with subtractive fixup: any nibble/byte that did NOT generate a carry from
// the binary add gets 6/$60 subtracted (in signed integer space — the result
// may go briefly negative before later carries swing it back). c_in follows
// 6502 borrow-in convention (C=1 means no borrow-in).
[[gnu::always_inline]] inline BcdResult BcdSub8(uint8_t a, uint8_t b, bool c_in) {
  const uint8_t nb = static_cast<uint8_t>(~b);
  // Use signed int so the "subtract 6" / "subtract $60" steps can briefly
  // go negative without unsigned-wrap noise; the partial-result tests are
  // signed-aware (`<= 0xF`, `<= 0xFF`) per the higan reference.
  int32_t result = static_cast<int32_t>(a & 0x0FU) + static_cast<int32_t>(nb & 0x0FU) + (c_in ? 1 : 0);
  if (result <= 0x0F) result -= 0x06;
  bool c = result > 0x0F;
  result = static_cast<int32_t>(a & 0xF0U) + static_cast<int32_t>(nb & 0xF0U) + (c ? 0x10 : 0) + (result & 0x0F);
  const bool v_out = ((~(a ^ nb) & (a ^ static_cast<uint8_t>(result))) & 0x80U) != 0U;
  if (result <= 0xFF) result -= 0x60;
  const bool c_out = result > 0xFF;
  const uint8_t final_result = static_cast<uint8_t>(static_cast<uint32_t>(result) & 0xFFU);
  return BcdResult{final_result, c_out, final_result == 0U, (final_result & 0x80U) != 0U, v_out};
}

[[gnu::always_inline]] inline BcdResult BcdAdd16(uint16_t a, uint16_t b, bool c_in) {
  // Per-nibble accumulation with fixup after each lower nibble. V is taken
  // after the third (bit-15-bearing) accumulation step, before the top
  // (>$9FFF -> +$6000) fixup. See file-level comment for the test 0404
  // counter-example to "V on raw binary sum".
  uint32_t result = (a & 0x000FU) + (b & 0x000FU) + (c_in ? 1U : 0U);
  if (result > 0x0009U) result += 0x0006U;
  bool c = result > 0x000FU;
  result = (a & 0x00F0U) + (b & 0x00F0U) + (c ? 0x0010U : 0U) + (result & 0x000FU);
  if (result > 0x009FU) result += 0x0060U;
  c = result > 0x00FFU;
  result = (a & 0x0F00U) + (b & 0x0F00U) + (c ? 0x0100U : 0U) + (result & 0x00FFU);
  if (result > 0x09FFU) result += 0x0600U;
  c = result > 0x0FFFU;
  result = (a & 0xF000U) + (b & 0xF000U) + (c ? 0x1000U : 0U) + (result & 0x0FFFU);
  const bool v_out = ((~(a ^ b) & (a ^ static_cast<uint16_t>(result))) & 0x8000U) != 0U;
  if (result > 0x9FFFU) result += 0x6000U;
  const bool c_out = result > 0xFFFFU;
  const uint16_t final_result = static_cast<uint16_t>(result & 0xFFFFU);
  return BcdResult{final_result, c_out, final_result == 0U, (final_result & 0x8000U) != 0U, v_out};
}

[[gnu::always_inline]] inline BcdResult BcdSub16(uint16_t a, uint16_t b, bool c_in) {
  const uint16_t nb = static_cast<uint16_t>(~b);
  int32_t result = static_cast<int32_t>(a & 0x000FU) + static_cast<int32_t>(nb & 0x000FU) + (c_in ? 1 : 0);
  if (result <= 0x000F) result -= 0x0006;
  bool c = result > 0x000F;
  result = static_cast<int32_t>(a & 0x00F0U) + static_cast<int32_t>(nb & 0x00F0U) + (c ? 0x0010 : 0) + (result & 0x000F);
  if (result <= 0x00FF) result -= 0x0060;
  c = result > 0x00FF;
  result =
      static_cast<int32_t>(a & 0x0F00U) + static_cast<int32_t>(nb & 0x0F00U) + (c ? 0x0100 : 0) + (result & 0x00FF);
  if (result <= 0x0FFF) result -= 0x0600;
  c = result > 0x0FFF;
  result =
      static_cast<int32_t>(a & 0xF000U) + static_cast<int32_t>(nb & 0xF000U) + (c ? 0x1000 : 0) + (result & 0x0FFF);
  const bool v_out = ((~(a ^ nb) & (a ^ static_cast<uint16_t>(result))) & 0x8000U) != 0U;
  if (result <= 0xFFFF) result -= 0x6000;
  const bool c_out = result > 0xFFFF;
  const uint16_t final_result = static_cast<uint16_t>(static_cast<uint32_t>(result) & 0xFFFFU);
  return BcdResult{final_result, c_out, final_result == 0U, (final_result & 0x8000U) != 0U, v_out};
}

}  // namespace pupsnes
