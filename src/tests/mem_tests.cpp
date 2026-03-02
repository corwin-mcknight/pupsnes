#include <catch2/catch_test_macros.hpp>

#include "pupsnes/mem.h"

TEST_CASE("wrapAddr masks to 24 bits", "[unit]") {
    using pupsnes::snes_addr_t;
    using pupsnes::util::wrapAddr;

    SECTION("leaves 24-bit addresses unchanged") {
        REQUIRE(wrapAddr(static_cast<snes_addr_t>(0x000000)) == 0x000000);
        REQUIRE(wrapAddr(static_cast<snes_addr_t>(0x00ABCD)) == 0x00ABCD);
        REQUIRE(wrapAddr(static_cast<snes_addr_t>(0xFFFFFF)) == 0xFFFFFF);
    }

    SECTION("clears bits above 24") {
        REQUIRE(wrapAddr(static_cast<snes_addr_t>(0x01000000)) == 0x000000);
        REQUIRE(wrapAddr(static_cast<snes_addr_t>(0x01FFFFFF)) == 0xFFFFFF);
        REQUIRE(wrapAddr(static_cast<snes_addr_t>(0x12345678)) == 0x345678);
    }
}
