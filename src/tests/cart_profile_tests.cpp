// Unit tests for DetectCartProfile.
//
// DetectCartProfile is a pure parser over raw cart bytes. It must classify
// the (mapper, coprocessor, sram, battery, rtc, region, fastrom) tuple from
// the SNES cartridge header at one of three offsets ($7FB0 LoROM, $FFB0
// HiROM, $40FFB0 ExHiROM), tolerating empty / truncated / corrupt inputs by
// returning kNone with a specific diagnostic.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <vector>

#include "pupsnes/hw/rom/cart_profile.h"
#include "pupsnes/hw/rom/rom_format.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)
using Catch::Matchers::ContainsSubstring;

namespace {

constexpr std::size_t kLoRomSize = 32U * 1024U;
constexpr std::size_t kHiRomSize = 1024U * 1024U;
// ExHiROM minimum: header at $40FFB0 requires at least 0x40FFE0 + 1 bytes of
// addressable space. Use 5 MiB so the header offsets sit comfortably inside.
constexpr std::size_t kExHiRomSize = 5U * 1024U * 1024U;

// LoROM image with a valid map-mode byte ($20) and a checksum/complement
// pair that XORs to $FFFF. SRAM size byte and chipset byte left at 0 so
// the resulting profile is the simplest possible (LoROM, no coproc, no
// SRAM, no battery, no RTC, Japan/NTSC).
std::vector<uint8_t> MakeValidLoRom() {
  std::vector<uint8_t> rom(kLoRomSize, 0xEAU);
  rom[kLoRomMapModeOffset] = 0x20U;
  rom[kLoRomChecksumComplementOffset] = 0x00U;
  rom[kLoRomChecksumComplementOffset + 1U] = 0x00U;
  rom[kLoRomChecksumOffset] = 0xFFU;
  rom[kLoRomChecksumOffset + 1U] = 0xFFU;
  rom[0x7FD6U] = 0x00U;  // chipset = plain ROM
  rom[0x7FD8U] = 0x00U;  // SRAM size = 0
  rom[0x7FD9U] = 0x00U;  // country = Japan (NTSC)
  return rom;
}

std::vector<uint8_t> MakeValidHiRom() {
  std::vector<uint8_t> rom(kHiRomSize, 0xEAU);
  rom[kHiRomMapModeOffset] = 0x21U;
  rom[kHiRomChecksumComplementOffset] = 0x00U;
  rom[kHiRomChecksumComplementOffset + 1U] = 0x00U;
  rom[kHiRomChecksumOffset] = 0xFFU;
  rom[kHiRomChecksumOffset + 1U] = 0xFFU;
  rom[0xFFD6U] = 0x00U;
  rom[0xFFD8U] = 0x00U;
  rom[0xFFD9U] = 0x01U;  // country = USA (NTSC)
  return rom;
}

// ExHiROM image with header at $40FFB0 and map-mode $25. To avoid ambiguity
// with HiROM detection we deliberately scrub the $FFD5 byte to a non-HiROM
// value (0xFF / open bus shape).
std::vector<uint8_t> MakeValidExHiRom() {
  std::vector<uint8_t> rom(kExHiRomSize, 0xEAU);
  rom[0xFFD5U] = 0xFFU;  // not a HiROM map-mode byte
  rom[0x40FFD5U] = 0x25U;
  rom[0x40FFDCU] = 0x00U;
  rom[0x40FFDCU + 1U] = 0x00U;
  rom[0x40FFDEU] = 0xFFU;
  rom[0x40FFDEU + 1U] = 0xFFU;
  rom[0x40FFD6U] = 0x00U;
  rom[0x40FFD8U] = 0x00U;
  rom[0x40FFD9U] = 0x02U;  // country = Europe (PAL)
  return rom;
}

}  // namespace

// ---------------------------------------------------------------------------
// Hard failure cases
// ---------------------------------------------------------------------------

TEST_CASE("DetectCartProfile: empty buffer reports kNone with empty diagnostic", "[unit][cart_profile]") {
  const std::vector<uint8_t> rom;
  const auto detection = DetectCartProfile(rom);
  REQUIRE(detection.profile.mapper == MapperKind::kNone);
  REQUIRE_THAT(detection.diagnostic, ContainsSubstring("empty"));
  REQUIRE(detection.profile.input_size == 0);
}

TEST_CASE("DetectCartProfile: SMC copier header is reported and refused", "[unit][cart_profile]") {
  // 32 KiB + 512 byte copier prefix — the size-modulo trigger.
  std::vector<uint8_t> rom(kLoRomSize + kSmcCopierHeaderSize, 0x00U);
  const auto detection = DetectCartProfile(rom);
  REQUIRE(detection.profile.mapper == MapperKind::kNone);
  REQUIRE(detection.profile.has_smc_copier_header);
  REQUIRE_THAT(detection.diagnostic, ContainsSubstring("copier"));
  REQUIRE_THAT(detection.diagnostic, ContainsSubstring("Strip"));
}

// ---------------------------------------------------------------------------
// LoROM / HiROM / ExHiROM detection
// ---------------------------------------------------------------------------

TEST_CASE("DetectCartProfile: valid LoROM is detected", "[unit][cart_profile]") {
  const auto rom = MakeValidLoRom();
  const auto detection = DetectCartProfile(rom);
  REQUIRE(detection.diagnostic.empty());
  REQUIRE(detection.profile.mapper == MapperKind::kLoROM);
  REQUIRE(detection.profile.coproc == Coprocessor::kNone);
  REQUIRE_FALSE(detection.profile.fastrom_capable);
  REQUIRE(detection.profile.lorom_checksum_valid);
  REQUIRE(detection.profile.input_size == kLoRomSize);
}

TEST_CASE("DetectCartProfile: valid HiROM is detected", "[unit][cart_profile]") {
  const auto rom = MakeValidHiRom();
  const auto detection = DetectCartProfile(rom);
  REQUIRE(detection.diagnostic.empty());
  REQUIRE(detection.profile.mapper == MapperKind::kHiROM);
  REQUIRE(detection.profile.coproc == Coprocessor::kNone);
  REQUIRE_FALSE(detection.profile.fastrom_capable);
  REQUIRE(detection.profile.hirom_checksum_valid);
  REQUIRE(detection.profile.region.country_code == 0x01U);
  REQUIRE_FALSE(detection.profile.region.is_pal);
}

TEST_CASE("DetectCartProfile: valid ExHiROM is detected", "[unit][cart_profile]") {
  const auto rom = MakeValidExHiRom();
  const auto detection = DetectCartProfile(rom);
  REQUIRE(detection.diagnostic.empty());
  REQUIRE(detection.profile.mapper == MapperKind::kExHiROM);
  REQUIRE(detection.profile.exhirom_checksum_valid);
  REQUIRE(detection.profile.region.country_code == 0x02U);
  REQUIRE(detection.profile.region.is_pal);
}

TEST_CASE("DetectCartProfile: small ROM cannot be ExHiROM even with $40FFD5 in range", "[unit][cart_profile]") {
  // A 32 KiB ROM is too short to hold an ExHiROM header at $40FFB0. Even if
  // some byte at index $40FFD5 magically matched, detection must refuse
  // ExHiROM because the header offset is past EOF.
  auto rom = MakeValidLoRom();
  const auto detection = DetectCartProfile(rom);
  REQUIRE(detection.profile.mapper == MapperKind::kLoROM);
  REQUIRE_FALSE(detection.profile.exhirom_checksum_valid);
}

// ---------------------------------------------------------------------------
// FastROM / coprocessor / battery / RTC / SRAM extraction
// ---------------------------------------------------------------------------

TEST_CASE("DetectCartProfile: FastROM flag is read from map-mode byte bit 4", "[unit][cart_profile]") {
  auto rom = MakeValidLoRom();
  rom[kLoRomMapModeOffset] = 0x30U;  // LoROM + FastROM
  const auto detection = DetectCartProfile(rom);
  REQUIRE(detection.profile.mapper == MapperKind::kLoROM);
  REQUIRE(detection.profile.fastrom_capable);
}

TEST_CASE("DetectCartProfile: chipset $03 yields DSP coprocessor", "[unit][cart_profile]") {
  auto rom = MakeValidLoRom();
  rom[0x7FD6U] = 0x03U;  // ROM+DSP
  const auto detection = DetectCartProfile(rom);
  REQUIRE(detection.profile.coproc == Coprocessor::kDSP);
  REQUIRE_FALSE(detection.profile.has_battery);
}

TEST_CASE("DetectCartProfile: chipset $35 yields SA-1 coprocessor with battery", "[unit][cart_profile]") {
  auto rom = MakeValidLoRom();
  rom[0x7FD6U] = 0x35U;  // ROM+SA1+RAM+Battery
  const auto detection = DetectCartProfile(rom);
  REQUIRE(detection.profile.coproc == Coprocessor::kSA1);
  REQUIRE(detection.profile.has_battery);
}

TEST_CASE("DetectCartProfile: chipset $55 yields S-RTC with battery + RTC", "[unit][cart_profile]") {
  auto rom = MakeValidLoRom();
  rom[0x7FD6U] = 0x55U;  // ROM+S-RTC+RAM+Battery
  const auto detection = DetectCartProfile(rom);
  REQUIRE(detection.profile.coproc == Coprocessor::kSRTC);
  REQUIRE(detection.profile.has_battery);
  REQUIRE(detection.profile.has_rtc);
}

TEST_CASE("DetectCartProfile: chipset $F5 + subtype $00 yields SPC7110", "[unit][cart_profile]") {
  auto rom = MakeValidLoRom();
  rom[0x7FD6U] = 0xF5U;  // ROM+Custom+RAM+Battery
  rom[0x7FBFU] = 0x00U;  // SPC7110 subtype
  const auto detection = DetectCartProfile(rom);
  REQUIRE(detection.profile.coproc == Coprocessor::kSPC7110);
  REQUIRE(detection.profile.custom_subtype == 0x00U);
}

TEST_CASE("DetectCartProfile: chipset $F3 + subtype $10 yields CX4", "[unit][cart_profile]") {
  auto rom = MakeValidLoRom();
  rom[0x7FD6U] = 0xF3U;
  rom[0x7FBFU] = 0x10U;
  const auto detection = DetectCartProfile(rom);
  REQUIRE(detection.profile.coproc == Coprocessor::kCX4);
  REQUIRE(detection.profile.custom_subtype == 0x10U);
}

TEST_CASE("DetectCartProfile: chipset $F6 + subtype $01 yields STxxx", "[unit][cart_profile]") {
  auto rom = MakeValidLoRom();
  rom[0x7FD6U] = 0xF6U;
  rom[0x7FBFU] = 0x01U;
  const auto detection = DetectCartProfile(rom);
  REQUIRE(detection.profile.coproc == Coprocessor::kSTxxx);
}

TEST_CASE("DetectCartProfile: SRAM size byte $03 yields 8 KiB", "[unit][cart_profile]") {
  auto rom = MakeValidLoRom();
  rom[0x7FD8U] = 0x03U;  // 1024 << 3 = 8 KiB
  const auto detection = DetectCartProfile(rom);
  REQUIRE(detection.profile.sram_bytes == 8U * 1024U);
}

TEST_CASE("DetectCartProfile: out-of-range SRAM size byte is clamped to zero", "[unit][cart_profile]") {
  auto rom = MakeValidLoRom();
  rom[0x7FD8U] = 0xFFU;
  const auto detection = DetectCartProfile(rom);
  REQUIRE(detection.profile.sram_bytes == 0);
}

// ---------------------------------------------------------------------------
// Tie-breaking when multiple candidates look valid
// ---------------------------------------------------------------------------

TEST_CASE("DetectCartProfile: HiROM wins when its checksum is the unique valid one", "[unit][cart_profile]") {
  // Build a HiROM-sized image where the LoROM map-mode byte ALSO accidentally
  // looks valid but only HiROM's checksum XORs to $FFFF. Detection should
  // pick HiROM.
  auto rom = MakeValidHiRom();
  rom[kLoRomMapModeOffset] = 0x20U;  // accidental LoROM-looking byte
  // LoROM checksum left invalid — the bytes around $7FDC are 0xEA filler.
  const auto detection = DetectCartProfile(rom);
  REQUIRE(detection.profile.mapper == MapperKind::kHiROM);
}

TEST_CASE("DetectCartProfile: defaults to LoROM when nothing discriminates", "[unit][cart_profile]") {
  // 32 KiB of 0xEA — no map-mode byte, no valid checksum. The fallback
  // should be LoROM (matches ld65 default; preserves legacy behaviour).
  std::vector<uint8_t> rom(kLoRomSize, 0xEAU);
  const auto detection = DetectCartProfile(rom);
  REQUIRE(detection.profile.mapper == MapperKind::kLoROM);
  REQUIRE(detection.diagnostic.empty());  // not a hard failure, just a fallback
}
