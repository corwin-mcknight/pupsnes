#pragma once

// BCD (decimal mode) helpers — D-08: no public API. Used by the kAdc/kSbc
// handler in ExecuteInternalOp when regs_.P.D is set. Algorithm derived from
// Bruce Clark's 65C816 reference (docs §6.1.1.1) and 01-RESEARCH.md §Research
// Priority 5.
//
// Deviation note (Rule 1 — algorithm bug fix against plan):
// The plan proposed handling SBC as `BcdAdd(A, ~B, C)`. That shortcut is a
// well-known binary-mode identity but it does NOT hold for BCD: the add-6 /
// add-$60 fixup assumes both operands are valid BCD digits, and ~B isn't.
// Concrete counter-example from this plan's own test suite: SBC $50 − $01
// with C=0 gave A=$B4 (expected $48); the 16-bit locked SC #4 fixture gave
// A=$4064 (expected $7998). The correct 65C816 behaviour uses *subtractive*
// fixup on the binary-SBC sum (subtract 6 when no nibble half-carry fired,
// subtract $60 when no full carry fired). We therefore keep BcdAdd{8,16} for
// ADC and add BcdSub{8,16} for SBC. The kAdc/kSbc dispatcher picks the right
// helper. Both share the same BcdResult shape so the call sites are uniform.
//
// BcdAdd16 / BcdSub16 are standalone full-width operations (HIGH-5): V is
// computed from the full 16-bit pre-adjustment binary sum, not from any
// intermediate fixup. The nibble fixup itself reuses the 8-bit helper for
// each byte (its value/carry outputs are correct); only its `overflow`
// output is discarded at 16 bits because 16-bit V follows a wider formula.
//
// Header-only inline definitions: BCD is a cold path (only entered with
// regs_.P.D set), but [[gnu::always_inline]] is preserved so the call sites
// in cpu.cpp keep their previous codegen shape.

#include <cstdint>

namespace pupsnes {

struct BcdResult {
  uint16_t value;  // adjusted result (8-bit path uses low 8 bits only)
  bool carry;      // C flag out
  bool zero;       // Z flag
  bool negative;   // N flag (bit 7 for 8-bit, bit 15 for 16-bit)
  bool overflow;   // V flag — computed from pre-adjustment binary sum
};

[[gnu::always_inline]] inline BcdResult BcdAdd8(uint8_t a, uint8_t b, bool c_in) {
  // 1. Binary sum (pre-fixup) — used for V.
  const uint16_t bin_sum = static_cast<uint16_t>(a) + static_cast<uint16_t>(b) + (c_in ? 1U : 0U);
  // 2. V from binary sum before fixup (standard 6502 ADC signed-overflow form).
  const bool v_out =
      ((~(static_cast<uint16_t>(a) ^ static_cast<uint16_t>(b)) & (static_cast<uint16_t>(a) ^ bin_sum)) & 0x80U) != 0U;
  // 3. Low-nibble fixup: if low nibble > 9 OR a nibble-carry fired, add 6.
  uint16_t adj = bin_sum;
  if ((adj & 0x0FU) > 9U || (bin_sum & 0x10U) != 0U) {
    adj = static_cast<uint16_t>(adj + 6U);
  }
  // 4. High-byte fixup + carry-out. Compare the byte-level adjusted value
  //    against $99 *and* honour any pre-existing binary carry out of bit 7
  //    so that e.g. $FE+$0D (invalid BCD but encountered via the SBC path in
  //    other designs) still reports C=1. For valid BCD inputs either
  //    discriminator suffices.
  bool c_out = (adj > 0x99U) || ((bin_sum & 0x100U) != 0U);
  if (c_out) {
    adj = static_cast<uint16_t>(adj + 0x60U);
  }
  const uint8_t result = static_cast<uint8_t>(adj & 0xFFU);
  return BcdResult{result, c_out, result == 0U, (result & 0x80U) != 0U, v_out};
}

// BCD subtraction 8-bit. Uses the binary SBC form (A + ~B + C_in) and applies
// *subtractive* fixup: subtract 6 from the byte when no nibble half-carry
// fired, subtract $60 when no full carry fired. C_in follows 6502 borrow-in
// convention (C=1 means no borrow-in).
[[gnu::always_inline]] inline BcdResult BcdSub8(uint8_t a, uint8_t b, bool c_in) {
  const uint16_t b_comp = static_cast<uint16_t>(static_cast<uint8_t>(~b) & 0xFFU);
  // 1. Binary SBC sum.
  const uint16_t bin_sum = static_cast<uint16_t>(a) + b_comp + (c_in ? 1U : 0U);
  // 2. V from binary SBC sum (uses ~B so form matches ADC formula on the
  //    complemented operand — this is the standard 6502 SBC V derivation).
  const bool v_out = ((~(static_cast<uint16_t>(a) ^ b_comp) & (static_cast<uint16_t>(a) ^ bin_sum)) & 0x80U) != 0U;
  // 3. Detect nibble half-carry from the *raw* nibble add (no BCD fixup yet).
  const bool half_carry = (((a & 0x0FU) + (b_comp & 0x0FU) + (c_in ? 1U : 0U)) & 0x10U) != 0U;
  // 4. Detect byte carry (same idea — straight off bin_sum).
  const bool full_carry = (bin_sum & 0x100U) != 0U;
  // 5. Subtractive fixup.
  uint16_t adj = bin_sum;
  if (!half_carry) {
    adj = static_cast<uint16_t>(adj - 6U);
  }
  if (!full_carry) {
    adj = static_cast<uint16_t>(adj - 0x60U);
  }
  const uint8_t result = static_cast<uint8_t>(adj & 0xFFU);
  // C_out for SBC = full_carry (no borrow = carry-out high).
  return BcdResult{result, full_carry, result == 0U, (result & 0x80U) != 0U, v_out};
}

[[gnu::always_inline]] inline BcdResult BcdAdd16(uint16_t a, uint16_t b, bool c_in) {
  // 1. Full 16-bit pre-adjustment binary sum. This is the value V is derived
  //    from — not any post-fixup intermediate, and NOT the high-byte
  //    BcdAdd8.overflow (that would compute V from a post-low-fixup carry
  //    chain, which is wrong at 16 bits). HIGH-5 correction.
  const uint32_t bin_sum = static_cast<uint32_t>(a) + static_cast<uint32_t>(b) + (c_in ? 1U : 0U);
  // 2. V from full 16-bit binary sum (ADC signed-overflow form).
  const bool v_out = ((a ^ static_cast<uint16_t>(bin_sum)) & (b ^ static_cast<uint16_t>(bin_sum)) & 0x8000U) != 0U;
  // 3. Apply BCD fixup digit-by-digit with carry propagation via BcdAdd8.
  const uint8_t lo_a = static_cast<uint8_t>(a & 0xFFU);
  const uint8_t lo_b = static_cast<uint8_t>(b & 0xFFU);
  const BcdResult lo = BcdAdd8(lo_a, lo_b, c_in);

  const uint8_t hi_a = static_cast<uint8_t>((a >> 8U) & 0xFFU);
  const uint8_t hi_b = static_cast<uint8_t>((b >> 8U) & 0xFFU);
  const BcdResult hi = BcdAdd8(hi_a, hi_b, lo.carry);

  const uint16_t result =
      static_cast<uint16_t>((static_cast<uint16_t>(hi.value) << 8U) | static_cast<uint16_t>(lo.value));
  return BcdResult{result, hi.carry, result == 0U, (result & 0x8000U) != 0U, v_out};
}

[[gnu::always_inline]] inline BcdResult BcdSub16(uint16_t a, uint16_t b, bool c_in) {
  // 1. Full 16-bit pre-adjustment binary SBC sum for the V flag.
  const uint32_t b_comp = static_cast<uint32_t>(static_cast<uint16_t>(~b) & 0xFFFFU);
  const uint32_t bin_sum = static_cast<uint32_t>(a) + b_comp + (c_in ? 1U : 0U);
  // 2. V from full 16-bit binary SBC sum.
  const uint16_t b_comp16 = static_cast<uint16_t>(b_comp);
  const bool v_out = ((~(a ^ b_comp16) & (a ^ static_cast<uint16_t>(bin_sum))) & 0x8000U) != 0U;
  // 3. Apply BCD subtractive fixup byte-by-byte via BcdSub8 with borrow prop.
  const uint8_t lo_a = static_cast<uint8_t>(a & 0xFFU);
  const uint8_t lo_b = static_cast<uint8_t>(b & 0xFFU);
  const BcdResult lo = BcdSub8(lo_a, lo_b, c_in);

  const uint8_t hi_a = static_cast<uint8_t>((a >> 8U) & 0xFFU);
  const uint8_t hi_b = static_cast<uint8_t>((b >> 8U) & 0xFFU);
  const BcdResult hi = BcdSub8(hi_a, hi_b, lo.carry);

  const uint16_t result =
      static_cast<uint16_t>((static_cast<uint16_t>(hi.value) << 8U) | static_cast<uint16_t>(lo.value));
  return BcdResult{result, hi.carry, result == 0U, (result & 0x8000U) != 0U, v_out};
}

}  // namespace pupsnes
