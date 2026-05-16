#include <catch2/catch_test_macros.hpp>

#include "pupsnes/memory/mem.h"

TEST_CASE("WrapAddr masks to 24 bits", "[unit]") {
  using pupsnes::SnesAddrT;
  using pupsnes::util::WrapAddr;

  SECTION("leaves 24-bit addresses unchanged") {
    REQUIRE(WrapAddr(static_cast<SnesAddrT>(0x000000)) == 0x000000);
    REQUIRE(WrapAddr(static_cast<SnesAddrT>(0x00ABCD)) == 0x00ABCD);
    REQUIRE(WrapAddr(static_cast<SnesAddrT>(0xFFFFFF)) == 0xFFFFFF);
  }

  SECTION("clears bits above 24") {
    REQUIRE(WrapAddr(static_cast<SnesAddrT>(0x01000000)) == 0x000000);
    REQUIRE(WrapAddr(static_cast<SnesAddrT>(0x01FFFFFF)) == 0xFFFFFF);
    REQUIRE(WrapAddr(static_cast<SnesAddrT>(0x12345678)) == 0x345678);
  }
}
