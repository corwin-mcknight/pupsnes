#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>

#include "pupsnes/debugger/sha1.h"

using pupsnes::debugger::Sha1;
using pupsnes::debugger::Sha1Digest;

namespace {

std::string ToHex(const Sha1Digest& d) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(40);
  for (uint8_t byte : d) {
    out.push_back(kHex[(byte >> 4) & 0x0F]);
    out.push_back(kHex[byte & 0x0F]);
  }
  return out;
}

Sha1Digest Hash(std::string_view s) {
  Sha1 h;
  h.Update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  return h.Finalize();
}

}  // namespace

TEST_CASE("SHA-1 matches RFC 3174 vector: abc", "[unit][debugger]") {
  CHECK(ToHex(Hash("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST_CASE("SHA-1 matches RFC 3174 vector: 448-bit block", "[unit][debugger]") {
  CHECK(ToHex(Hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST_CASE("SHA-1 matches RFC 3174 vector: empty input", "[unit][debugger]") {
  CHECK(ToHex(Hash("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST_CASE("SHA-1 Update can be called in chunks", "[unit][debugger]") {
  Sha1 h;
  h.Update(reinterpret_cast<const uint8_t*>("a"), 1);
  h.Update(reinterpret_cast<const uint8_t*>("b"), 1);
  h.Update(reinterpret_cast<const uint8_t*>("c"), 1);
  CHECK(ToHex(h.Finalize()) == "a9993e364706816aba3e25717850c26c9cd0d89d");
}
