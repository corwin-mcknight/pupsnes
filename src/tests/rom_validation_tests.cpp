// ROM load-time validation tests.
//
// Make sure every shape of "wrong" cartridge image is rejected with a
// specific, user-facing message — the LoadLoRom / LoadHiRom / LoadRom API
// is the only path to a mapped cartridge, so anything ambiguous or
// inconsistent has to bounce off here with enough detail for the user (or
// the debugger error log) to fix.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <vector>

#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/hw/rom/rom_format.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)
using Catch::Matchers::ContainsSubstring;

namespace {

constexpr std::size_t kLoRomSize = 32U * 1024U;
constexpr std::size_t kHiRomSize = 1U * 1024U * 1024U;

// Build a LoROM that satisfies map-mode + checksum validation: byte $7FD5
// gets $20, and the checksum / complement pair XORs to $FFFF.
std::vector<uint8_t> MakeValidLoRom() {
  std::vector<uint8_t> rom(kLoRomSize, 0xEAU);
  rom[kLoRomMapModeOffset] = 0x20U;  // LoROM, slow
  rom[kLoRomChecksumComplementOffset] = 0x00U;
  rom[kLoRomChecksumComplementOffset + 1U] = 0x00U;
  rom[kLoRomChecksumOffset] = 0xFFU;
  rom[kLoRomChecksumOffset + 1U] = 0xFFU;
  rom[0x7FFCU] = 0x00U;  // Reset vector $8000
  rom[0x7FFDU] = 0x80U;
  return rom;
}

std::vector<uint8_t> MakeValidHiRom() {
  std::vector<uint8_t> rom(kHiRomSize, 0xEAU);
  rom[kHiRomMapModeOffset] = 0x21U;  // HiROM, slow
  rom[kHiRomChecksumComplementOffset] = 0x00U;
  rom[kHiRomChecksumComplementOffset + 1U] = 0x00U;
  rom[kHiRomChecksumOffset] = 0xFFU;
  rom[kHiRomChecksumOffset + 1U] = 0xFFU;
  rom[0xFFFCU] = 0x00U;
  rom[0xFFFDU] = 0x80U;
  return rom;
}

}  // namespace

// ---------------------------------------------------------------------------
// Hard-shape rejections
// ---------------------------------------------------------------------------

TEST_CASE("LoadLoRom: empty buffer is rejected with a specific message", "[unit][rom_validation]") {
  SNES snes;
  std::vector<uint8_t> rom;
  const RomLoadResult result = snes.LoadLoRom(rom);
  REQUIRE_FALSE(result.ok);
  REQUIRE_THAT(result.message, ContainsSubstring("empty"));
  REQUIRE(snes.GetCartridge().GetMapperKind() == MapperKind::kNone);
}

TEST_CASE("LoadHiRom: empty buffer is rejected", "[unit][rom_validation]") {
  SNES snes;
  std::vector<uint8_t> rom;
  const RomLoadResult result = snes.LoadHiRom(rom);
  REQUIRE_FALSE(result.ok);
  REQUIRE_THAT(result.message, ContainsSubstring("empty"));
}

TEST_CASE("LoadRom: empty buffer is rejected with a specific message", "[unit][rom_validation]") {
  SNES snes;
  std::vector<uint8_t> rom;
  const RomLoadResult result = snes.LoadRom(rom);
  REQUIRE_FALSE(result.ok);
  REQUIRE_THAT(result.message, ContainsSubstring("empty"));
}

TEST_CASE("LoadLoRom: ROM shorter than LoROM header is rejected with a size hint", "[unit][rom_validation]") {
  SNES snes;
  std::vector<uint8_t> rom(1024U, 0xFFU);  // 1 KiB — way below $7FD5
  const RomLoadResult result = snes.LoadLoRom(rom);
  REQUIRE_FALSE(result.ok);
  REQUIRE_THAT(result.message, ContainsSubstring("too small"));
  REQUIRE_THAT(result.message, ContainsSubstring("LoROM"));
}

TEST_CASE("LoadHiRom: ROM shorter than HiROM header is rejected", "[unit][rom_validation]") {
  SNES snes;
  std::vector<uint8_t> rom(kLoRomSize, 0xFFU);  // 32 KiB — never reaches $FFD5
  const RomLoadResult result = snes.LoadHiRom(rom);
  REQUIRE_FALSE(result.ok);
  REQUIRE_THAT(result.message, ContainsSubstring("too small"));
  REQUIRE_THAT(result.message, ContainsSubstring("HiROM"));
}

TEST_CASE("LoadLoRom: SMC copier header is detected and named in the error", "[unit][rom_validation]") {
  SNES snes;
  // 32 KiB LoROM + 512-byte copier header = size mod 32 KiB == 512.
  std::vector<uint8_t> rom(kLoRomSize + kSmcCopierHeaderSize, 0xFFU);
  const RomLoadResult result = snes.LoadLoRom(rom);
  REQUIRE_FALSE(result.ok);
  REQUIRE_THAT(result.message, ContainsSubstring("SMC copier header"));
  REQUIRE_THAT(result.message, ContainsSubstring("StripSmcCopierHeader"));
}

TEST_CASE("LoadRom: SMC copier header is rejected before mapper dispatch", "[unit][rom_validation]") {
  SNES snes;
  std::vector<uint8_t> rom(kLoRomSize + kSmcCopierHeaderSize, 0xFFU);
  const RomLoadResult result = snes.LoadRom(rom);
  REQUIRE_FALSE(result.ok);
  REQUIRE_THAT(result.message, ContainsSubstring("SMC copier header"));
}

// ---------------------------------------------------------------------------
// Mapper-mismatch rejections (soft validation)
// ---------------------------------------------------------------------------

TEST_CASE("LoadLoRom: HiROM-flavoured header is refused with a use-LoadHiRom hint", "[unit][rom_validation]") {
  SNES snes;
  // Build a buffer that looks more like HiROM than LoROM: HiROM map mode
  // byte is in range AND HiROM checksum is valid; LoROM byte and checksum
  // are both unset. The validator must flag this and refuse the LoROM load.
  std::vector<uint8_t> rom = MakeValidHiRom();
  rom[kLoRomMapModeOffset] = 0xFFU;  // Make sure LoROM map mode is clearly invalid.

  const RomLoadResult result = snes.LoadLoRom(rom);
  REQUIRE_FALSE(result.ok);
  REQUIRE(result.detected_kind == MapperKind::kHiROM);
  REQUIRE_THAT(result.message, ContainsSubstring("HiROM"));
  REQUIRE_THAT(result.message, ContainsSubstring("LoadHiRom"));
  // Detected map-mode byte should be quoted in the message so the user can
  // verify the header inspection from the error alone.
  REQUIRE_THAT(result.message, ContainsSubstring("0x21"));
  // Cartridge state must NOT have been mutated by the rejected load.
  REQUIRE(snes.GetCartridge().GetMapperKind() == MapperKind::kNone);
}

TEST_CASE("LoadHiRom: LoROM-flavoured header is refused with a use-LoadLoRom hint", "[unit][rom_validation]") {
  SNES snes;
  // Big enough buffer to clear the HiROM hard-size check, with a clearly
  // LoROM header at $7FD5 and the HiROM header byte set to something
  // outside the valid range.
  std::vector<uint8_t> rom(kHiRomSize, 0xEAU);
  rom[kLoRomMapModeOffset] = 0x20U;  // Valid LoROM map mode
  rom[kLoRomChecksumComplementOffset] = 0x00U;
  rom[kLoRomChecksumComplementOffset + 1U] = 0x00U;
  rom[kLoRomChecksumOffset] = 0xFFU;
  rom[kLoRomChecksumOffset + 1U] = 0xFFU;
  rom[kHiRomMapModeOffset] = 0xFFU;  // Clearly NOT HiROM

  const RomLoadResult result = snes.LoadHiRom(rom);
  REQUIRE_FALSE(result.ok);
  REQUIRE(result.detected_kind == MapperKind::kLoROM);
  REQUIRE_THAT(result.message, ContainsSubstring("LoROM"));
  REQUIRE_THAT(result.message, ContainsSubstring("LoadLoRom"));
  REQUIRE_THAT(result.message, ContainsSubstring("0x20"));
}

TEST_CASE("Cartridge state is preserved when a load is rejected", "[unit][rom_validation]") {
  SNES snes;
  // Load a valid LoROM first.
  const RomLoadResult first = snes.LoadLoRom(MakeValidLoRom());
  REQUIRE(first.ok);
  REQUIRE(snes.GetCartridge().GetMapperKind() == MapperKind::kLoROM);
  const std::size_t first_size = snes.GetCartridge().Size();

  // Then try to load garbage — should not disturb the live cartridge.
  std::vector<uint8_t> bogus;
  const RomLoadResult second = snes.LoadLoRom(bogus);
  REQUIRE_FALSE(second.ok);
  REQUIRE(snes.GetCartridge().GetMapperKind() == MapperKind::kLoROM);
  REQUIRE(snes.GetCartridge().Size() == first_size);
}

// ---------------------------------------------------------------------------
// Auto-detect dispatch via SNES::LoadRom
// ---------------------------------------------------------------------------

TEST_CASE("LoadRom: valid LoROM header routes through LoadLoRom", "[unit][rom_validation]") {
  SNES snes;
  const RomLoadResult result = snes.LoadRom(MakeValidLoRom());
  REQUIRE(result.ok);
  REQUIRE(result.detected_kind == MapperKind::kLoROM);
  REQUIRE(snes.GetCartridge().GetMapperKind() == MapperKind::kLoROM);
}

TEST_CASE("LoadRom: valid HiROM header routes through LoadHiRom", "[unit][rom_validation]") {
  SNES snes;
  const RomLoadResult result = snes.LoadRom(MakeValidHiRom());
  REQUIRE(result.ok);
  REQUIRE(result.detected_kind == MapperKind::kHiROM);
  REQUIRE(snes.GetCartridge().GetMapperKind() == MapperKind::kHiROM);
}

TEST_CASE("LoadRom: ambiguous / headerless ROM falls back to LoROM", "[unit][rom_validation]") {
  // Homebrew + ld65-built test ROMs commonly leave $7FD5 and $FFD5 unset —
  // neither map mode byte is in range, neither checksum is valid. The
  // detector defaults to LoROM in this case, since that's what the standard
  // ld65 LoROM linker config emits.
  SNES snes;
  std::vector<uint8_t> rom(kLoRomSize, 0xEAU);
  rom[0x7FFCU] = 0x00U;
  rom[0x7FFDU] = 0x80U;

  const RomLoadResult result = snes.LoadRom(rom);
  REQUIRE(result.ok);
  REQUIRE(result.detected_kind == MapperKind::kLoROM);
}

// ---------------------------------------------------------------------------
// Header inspection helpers (rom_format.h)
// ---------------------------------------------------------------------------

TEST_CASE("IsLoRomMapModeByte / IsHiRomMapModeByte recognise documented map modes", "[unit][rom_format]") {
  REQUIRE(IsLoRomMapModeByte(0x20U));
  REQUIRE(IsLoRomMapModeByte(0x30U));
  REQUIRE_FALSE(IsLoRomMapModeByte(0x21U));
  REQUIRE_FALSE(IsLoRomMapModeByte(0x32U));  // SA-1 — intentionally excluded
  REQUIRE(IsHiRomMapModeByte(0x21U));
  REQUIRE(IsHiRomMapModeByte(0x31U));
  REQUIRE_FALSE(IsHiRomMapModeByte(0x25U));  // ExHiROM — intentionally excluded
}

TEST_CASE("LoRomChecksumValid / HiRomChecksumValid match when complement XOR checksum == $FFFF", "[unit][rom_format]") {
  std::vector<uint8_t> rom = MakeValidLoRom();
  REQUIRE(LoRomChecksumValid(rom));
  // Flip one bit of the checksum — the XOR is no longer $FFFF.
  rom[kLoRomChecksumOffset] ^= 0x01U;
  REQUIRE_FALSE(LoRomChecksumValid(rom));

  std::vector<uint8_t> hi = MakeValidHiRom();
  REQUIRE(HiRomChecksumValid(hi));
  hi[kHiRomChecksumOffset] ^= 0x80U;
  REQUIRE_FALSE(HiRomChecksumValid(hi));
}
