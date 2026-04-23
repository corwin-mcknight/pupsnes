#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "pupsnes/rom_format.h"

using pupsnes::HasSmcCopierHeader;
using pupsnes::kLoRomBankSize;
using pupsnes::kSmcCopierHeaderSize;
using pupsnes::StripSmcCopierHeader;

namespace {

std::vector<uint8_t> MakeBuffer(std::size_t size, uint8_t fill) { return std::vector<uint8_t>(size, fill); }

}  // namespace

TEST_CASE("HasSmcCopierHeader: detects 512-byte copier prefix on LoROM sizes", "[unit][rom_format]") {
  // Super Mario World shape: 512 KiB image + 512-byte copier header.
  REQUIRE(HasSmcCopierHeader(kLoRomBankSize * 16U + kSmcCopierHeaderSize));
  // Kirby Super Star shape: 4 MiB image + header. Uses .sfc in our roms dir
  // but the detection is purely size-based and must still recognise a
  // hypothetical .smc-style prefix on a 4 MiB ROM.
  REQUIRE(HasSmcCopierHeader(kLoRomBankSize * 128U + kSmcCopierHeaderSize));
}

TEST_CASE("HasSmcCopierHeader: rejects header-less ROM images", "[unit][rom_format]") {
  REQUIRE_FALSE(HasSmcCopierHeader(kLoRomBankSize));
  REQUIRE_FALSE(HasSmcCopierHeader(kLoRomBankSize * 16U));
  REQUIRE_FALSE(HasSmcCopierHeader(kLoRomBankSize * 128U));
}

TEST_CASE("HasSmcCopierHeader: rejects sub-bank remainders that are not 512", "[unit][rom_format]") {
  REQUIRE_FALSE(HasSmcCopierHeader(kLoRomBankSize + 1U));
  REQUIRE_FALSE(HasSmcCopierHeader(kLoRomBankSize + 256U));
  REQUIRE_FALSE(HasSmcCopierHeader(kLoRomBankSize + 1024U));
  REQUIRE_FALSE(HasSmcCopierHeader(0U));
}

TEST_CASE("StripSmcCopierHeader: removes prefix and preserves payload", "[unit][rom_format]") {
  // Build a recognisable payload: first byte of the ROM body is 0xAA, last
  // byte is 0x55. The copier-header region is 0xFF so a failure to strip
  // would show up immediately in the first payload byte.
  std::vector<uint8_t> rom = MakeBuffer(kSmcCopierHeaderSize, 0xFFU);
  std::vector<uint8_t> body = MakeBuffer(kLoRomBankSize * 16U, 0x00U);
  body.front() = 0xAAU;
  body.back() = 0x55U;
  rom.insert(rom.end(), body.begin(), body.end());

  REQUIRE(rom.size() == kLoRomBankSize * 16U + kSmcCopierHeaderSize);

  StripSmcCopierHeader(rom);

  REQUIRE(rom.size() == kLoRomBankSize * 16U);
  REQUIRE(rom.front() == 0xAAU);
  REQUIRE(rom.back() == 0x55U);
}

TEST_CASE("StripSmcCopierHeader: leaves header-less ROMs untouched", "[unit][rom_format]") {
  std::vector<uint8_t> rom = MakeBuffer(kLoRomBankSize * 16U, 0x42U);
  const std::size_t original_size = rom.size();

  StripSmcCopierHeader(rom);

  REQUIRE(rom.size() == original_size);
  REQUIRE(rom.front() == 0x42U);
}
