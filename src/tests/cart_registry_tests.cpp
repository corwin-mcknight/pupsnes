// CartridgeRegistry unit tests.
//
// The registry is the central dispatch point for cart construction. Tests
// cover:
//   1. Built-in (LoROM, HiROM, ExHiROM) builders are registered after
//      SNES construction.
//   2. Building an unknown (mapper, coproc) returns a helpful diagnostic.
//   3. LoadRomWithProfile lets callers force a mapper kind, overriding
//      auto-detection (the homebrew / broken-header use case).
//   4. Register() replaces existing entries (tests can inject fakes).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <vector>

#include "pupsnes/hw/rom/cart_profile.h"
#include "pupsnes/hw/rom/cart_registry.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/hw/rom/rom_format.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)
using Catch::Matchers::ContainsSubstring;

namespace {

std::vector<uint8_t> MakeValidLoRom() {
  std::vector<uint8_t> rom(32U * 1024U, 0xEAU);
  rom[kLoRomMapModeOffset] = 0x20U;
  rom[0x7FD6U] = 0x00U;  // chipset byte: plain ROM (zero out the 0xEA filler)
  rom[kLoRomChecksumComplementOffset] = 0x00U;
  rom[kLoRomChecksumComplementOffset + 1U] = 0x00U;
  rom[kLoRomChecksumOffset] = 0xFFU;
  rom[kLoRomChecksumOffset + 1U] = 0xFFU;
  return rom;
}

std::vector<uint8_t> MakeValidHiRom() {
  std::vector<uint8_t> rom(1U * 1024U * 1024U, 0xEAU);
  rom[kHiRomMapModeOffset] = 0x21U;
  rom[0xFFD6U] = 0x00U;
  rom[kHiRomChecksumComplementOffset] = 0x00U;
  rom[kHiRomChecksumComplementOffset + 1U] = 0x00U;
  rom[kHiRomChecksumOffset] = 0xFFU;
  rom[kHiRomChecksumOffset + 1U] = 0xFFU;
  return rom;
}

}  // namespace

// ---------------------------------------------------------------------------
// Built-in builders are registered
// ---------------------------------------------------------------------------

TEST_CASE("CartridgeRegistry: LoROM builder is registered after construction", "[unit][cart_registry]") {
  SNES snes;
  const auto rom = MakeValidLoRom();
  const auto profile = DetectCartProfile(rom).profile;
  const BuildResult result = snes.LoadRomWithProfile(profile, rom);
  REQUIRE(result.ok);
  REQUIRE(result.profile.mapper == MapperKind::kLoROM);
}

TEST_CASE("CartridgeRegistry: HiROM builder is registered after construction", "[unit][cart_registry]") {
  SNES snes;
  const auto rom = MakeValidHiRom();
  const auto profile = DetectCartProfile(rom).profile;
  const BuildResult result = snes.LoadRomWithProfile(profile, rom);
  REQUIRE(result.ok);
  REQUIRE(result.profile.mapper == MapperKind::kHiROM);
}

// ---------------------------------------------------------------------------
// Unknown coprocessor returns a helpful diagnostic
// ---------------------------------------------------------------------------

TEST_CASE("CartridgeRegistry: (LoROM, SA-1) returns 'not yet supported' message", "[unit][cart_registry]") {
  SNES snes;
  auto rom = MakeValidLoRom();
  CartProfile profile = DetectCartProfile(rom).profile;
  profile.coproc = Coprocessor::kSA1;  // Force SA-1 dispatch — no builder yet.
  const BuildResult result = snes.LoadRomWithProfile(profile, rom);
  REQUIRE_FALSE(result.ok);
  REQUIRE_THAT(result.message, ContainsSubstring("SA-1"));
  REQUIRE_THAT(result.message, ContainsSubstring("not yet implemented"));
}

TEST_CASE("CartridgeRegistry: (HiROM, SPC7110) returns 'not yet supported' message", "[unit][cart_registry]") {
  SNES snes;
  auto rom = MakeValidHiRom();
  CartProfile profile = DetectCartProfile(rom).profile;
  profile.coproc = Coprocessor::kSPC7110;
  const BuildResult result = snes.LoadRomWithProfile(profile, rom);
  REQUIRE_FALSE(result.ok);
  REQUIRE_THAT(result.message, ContainsSubstring("SPC7110"));
}

// ---------------------------------------------------------------------------
// Force-override workflow: homebrew with broken header
// ---------------------------------------------------------------------------

TEST_CASE("CartridgeRegistry: caller can force HiROM on a LoROM-shaped image", "[unit][cart_registry]") {
  SNES snes;
  // A bare LoROM image — auto-detection would pick LoROM. The caller can
  // override by mutating the profile before calling LoadRomWithProfile.
  // This won't necessarily produce a *runnable* cart (the LoROM image
  // doesn't have a valid HiROM header), but the registry must still
  // dispatch to the HiROM builder.
  const auto rom = MakeValidLoRom();
  CartProfile profile = DetectCartProfile(rom).profile;
  REQUIRE(profile.mapper == MapperKind::kLoROM);
  profile.mapper = MapperKind::kHiROM;
  const BuildResult result = snes.LoadRomWithProfile(profile, rom);
  // Build will fail because the bytes don't look like HiROM, but the
  // registry's dispatch went to the HiROM builder — the failure message
  // comes from HiROM-side validation, not the registry.
  REQUIRE_FALSE(result.ok);
  REQUIRE_THAT(result.message, ContainsSubstring("HiROM"));
}

// ---------------------------------------------------------------------------
// Register() lets tests inject fake builders
// ---------------------------------------------------------------------------

TEST_CASE("CartridgeRegistry::Register replaces an existing builder", "[unit][cart_registry]") {
  SNES snes;
  bool fake_builder_was_called = false;
  snes.Registry().Register(
      {MapperKind::kLoROM, Coprocessor::kNone},
      [&fake_builder_was_called](SNES* /*snes*/, std::span<const uint8_t> /*bytes*/, const CartProfile& profile) {
        fake_builder_was_called = true;
        return BuildResult{true, nullptr, profile, "fake builder ran"};
      });

  const auto rom = MakeValidLoRom();
  const auto profile = DetectCartProfile(rom).profile;
  const BuildResult result = snes.LoadRomWithProfile(profile, rom);
  REQUIRE(result.ok);
  REQUIRE(fake_builder_was_called);
  REQUIRE(result.message == "fake builder ran");
}
