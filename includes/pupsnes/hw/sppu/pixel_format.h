#pragma once

#include <cstddef>
#include <cstdint>

// Canonical SNES PPU output conversion. The PPU emits BGR555 (5:5:5) pixels;
// every front-end (emulator window, debugger preview, screenshot tool) needs to
// expand those to 8-bit-per-channel for display or file output. Keeping the
// conversion here means a colour/gamma/brightness correctness fix lands in
// exactly one place instead of drifting across three reimplementations.

namespace pupsnes::sppu {

// Maximum logical PPU framebuffer (256 wide × 239 with overscan). The unused
// tail when overscan is off costs nothing.
inline constexpr std::size_t kMaxLogicalPixels = 256U * 239U;

// 5-to-8 bit expansion: replicate the high bits into the low bits so the 5-bit
// value 31 maps to 255, not 248. The low 5 bits of `v5` are used; higher bits
// are ignored, so callers may pass an unshifted/unmasked channel field.
constexpr uint32_t Expand5To8(uint32_t v5) {
  const uint32_t c = v5 & 0x1FU;
  return (c << 3U) | (c >> 2U);
}

// Convert a BGR555 pixel to packed little-endian RGBA8 ([R][G][B][A] in memory).
// IM_COL32 and glTexSubImage2D(GL_RGBA, GL_UNSIGNED_BYTE) both expect this byte
// order on little-endian hosts.
constexpr uint32_t Bgr555ToRgba8(uint16_t c) {
  const uint32_t r = Expand5To8(static_cast<uint32_t>(c));
  const uint32_t g = Expand5To8(static_cast<uint32_t>(c) >> 5U);
  const uint32_t b = Expand5To8(static_cast<uint32_t>(c) >> 10U);
  return r | (g << 8U) | (b << 16U) | (0xFFU << 24U);
}

}  // namespace pupsnes::sppu
