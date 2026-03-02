#include <catch2/catch_test_macros.hpp>

#include "pupsnes/mem.h"

TEST_CASE("wrap_addr masks to 24 bits", "[unit]") {
    using pupsnes::snes_addr_t;
    using pupsnes::util::wrap_addr;

    SECTION("leaves 24-bit addresses unchanged") {
        REQUIRE(wrap_addr(static_cast<snes_addr_t>(0x000000)) == 0x000000);
        REQUIRE(wrap_addr(static_cast<snes_addr_t>(0x00ABCD)) == 0x00ABCD);
        REQUIRE(wrap_addr(static_cast<snes_addr_t>(0xFFFFFF)) == 0xFFFFFF);
    }

    SECTION("clears bits above 24") {
        REQUIRE(wrap_addr(static_cast<snes_addr_t>(0x01000000)) == 0x000000);
        REQUIRE(wrap_addr(static_cast<snes_addr_t>(0x01FFFFFF)) == 0xFFFFFF);
        REQUIRE(wrap_addr(static_cast<snes_addr_t>(0x12345678)) == 0x345678);
    }
}
