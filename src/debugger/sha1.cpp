#include "pupsnes/debugger/sha1.h"

#include <algorithm>
#include <cstring>

namespace pupsnes::debugger {

namespace {
constexpr uint32_t RotL(uint32_t x, int n) {
  return (x << n) | (x >> (32 - n));
}
}  // namespace

Sha1::Sha1() {
  h_[0] = 0x67452301U;
  h_[1] = 0xEFCDAB89U;
  h_[2] = 0x98BADCFEU;
  h_[3] = 0x10325476U;
  h_[4] = 0xC3D2E1F0U;
}

void Sha1::Update(const uint8_t* data, std::size_t len) {
  total_bits_ += static_cast<uint64_t>(len) * 8U;
  while (len > 0) {
    const std::size_t take = std::min<std::size_t>(64 - block_used_, len);
    std::memcpy(block_ + block_used_, data, take);
    block_used_ += take;
    data += take;
    len -= take;
    if (block_used_ == 64) {
      ProcessBlock();
      block_used_ = 0;
    }
  }
}

void Sha1::ProcessBlock() {
  uint32_t w[80];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block_[i * 4]) << 24) |
           (static_cast<uint32_t>(block_[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block_[i * 4 + 2]) << 8) |
           (static_cast<uint32_t>(block_[i * 4 + 3]));
  }
  for (int i = 16; i < 80; ++i) {
    w[i] = RotL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }

  uint32_t a = h_[0];
  uint32_t b = h_[1];
  uint32_t c = h_[2];
  uint32_t d = h_[3];
  uint32_t e = h_[4];

  for (int i = 0; i < 80; ++i) {
    uint32_t f;
    uint32_t k;
    if (i < 20) {
      f = (b & c) | ((~b) & d);
      k = 0x5A827999U;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1U;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCU;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6U;
    }
    const uint32_t temp = RotL(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = RotL(b, 30);
    b = a;
    a = temp;
  }

  h_[0] += a;
  h_[1] += b;
  h_[2] += c;
  h_[3] += d;
  h_[4] += e;
}

Sha1Digest Sha1::Finalize() {
  const uint64_t bits = total_bits_;
  const uint8_t pad = 0x80U;
  Update(&pad, 1);

  while (block_used_ != 56) {
    const uint8_t zero = 0;
    Update(&zero, 1);
  }

  uint8_t len_be[8];
  for (std::size_t i = 0; i < 8; ++i) {
    len_be[i] = static_cast<uint8_t>((bits >> ((7 - i) * 8)) & 0xFFU);
  }
  Update(len_be, 8);

  Sha1Digest out{};
  for (std::size_t i = 0; i < 5; ++i) {
    out[i * 4] = static_cast<uint8_t>((h_[i] >> 24) & 0xFFU);
    out[i * 4 + 1] = static_cast<uint8_t>((h_[i] >> 16) & 0xFFU);
    out[i * 4 + 2] = static_cast<uint8_t>((h_[i] >> 8) & 0xFFU);
    out[i * 4 + 3] = static_cast<uint8_t>(h_[i] & 0xFFU);
  }
  return out;
}

}  // namespace pupsnes::debugger
