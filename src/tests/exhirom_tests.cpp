// ExHiROM mapper test suite. Mirrors hirom_tests.cpp's structure for the
// extended-HiROM layout used by Tales of Phantasia and Dai Kaiju Monogatari 2.
//
// Key contract: banks $80-$BF (pages $80-$FF) and $C0-$FF (pages $00-$FF) see
// ROM bytes starting at file offset $000000; banks $00-$3F (pages $80-$FF)
// and $40-$7D (pages $00-$FF) see ROM bytes starting at file offset $400000.
// Smaller-half mirroring applies inside the lower-bank range when the cart is
// less than 8 MiB. FASTROM affects only the upper-bank ($80-$FF) range.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <vector>

#include "pupsnes/hw/5a22/cpu_mmio.h"
#include "pupsnes/hw/rom/cart_profile.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/hw/rom/rom_format.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/memory/systembus.h"
#include "systembus_test_access.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)
using Catch::Matchers::ContainsSubstring;

namespace {

// Smallest "real" ExHiROM has to cover at least the header at $40FFB0. The
// canonical commercial sizes are 5 MiB (Dai Kaiju 2) and 6 MiB (Tales of
// Phantasia). 8 MiB (4 MiB upper + 4 MiB lower) is the cleanest test fixture:
// no smaller-half mirroring, fast pointer windows fit every page.
constexpr std::size_t kExHiRom8M = 8U * 1024U * 1024U;
// 6 MiB layout: 4 MiB upper + 2 MiB lower. Lower banks $40-$5F cover the
// smaller half; banks $60-$7D mirror $40-$5F. The lower-bank pages past
// the 6 MiB mark exercise the wrap-around formula.
constexpr std::size_t kExHiRom6M = 6U * 1024U * 1024U;

constexpr std::size_t kExHiRomChecksumComplementOffset = 0x40FFDCU;
constexpr std::size_t kExHiRomChecksumOffset = 0x40FFDEU;

// Build an ExHiROM image. Each ROM byte is set to ((offset >> 8) ^ 0x5A) so
// the page table's chosen base_offset can be verified by reading one byte
// per page through the bus.
std::vector<uint8_t> MakeExHiRom(std::size_t size, uint8_t sram_byte = 0x00U) {
  std::vector<uint8_t> rom(size, 0xEAU);
  for (std::size_t i = 0; i < rom.size(); ++i) {
    rom[i] = static_cast<uint8_t>(((i >> 8) & 0xFFU) ^ 0x5AU);
  }
  // Scrub the HiROM map-mode byte so detection doesn't see a HiROM-looking
  // header at $FFD5. We want detection to fall through to ExHiROM.
  rom[0xFFD5U] = 0xFFU;
  // ExHiROM header at $40FFB0.
  rom[kExHiRomMapModeOffset] = 0x25U;
  rom[kExHiRomChecksumComplementOffset] = 0x00U;
  rom[kExHiRomChecksumComplementOffset + 1U] = 0x00U;
  rom[kExHiRomChecksumOffset] = 0xFFU;
  rom[kExHiRomChecksumOffset + 1U] = 0xFFU;
  rom[kExHiRomSramSizeOffset] = sram_byte;
  return rom;
}

const PageTableEntry& GetEntry(const SNES& snes, uint8_t bank, uint8_t page) {
  return SystemBusTestAccess::GetPageEntry(*snes.system_bus, bank, page);
}

}  // namespace

// ---------------------------------------------------------------------------
// Validation: LoadRom + force-override error paths
// ---------------------------------------------------------------------------

TEST_CASE("LoadRom: empty buffer is rejected with empty diagnostic", "[unit][exhirom]") {
  SNES snes;
  const BuildResult result = snes.LoadRom(std::vector<uint8_t>{});
  REQUIRE_FALSE(result.ok);
  REQUIRE_THAT(result.message, ContainsSubstring("empty"));
}

TEST_CASE("LoadRomWithProfile: forced kExHiROM on too-small image is rejected", "[unit][exhirom]") {
  SNES snes;
  std::vector<uint8_t> rom(64U * 1024U, 0xEAU);
  CartProfile forced{};
  forced.mapper = MapperKind::kExHiROM;
  forced.coproc = Coprocessor::kNone;
  const BuildResult result = snes.LoadRomWithProfile(forced, rom);
  REQUIRE_FALSE(result.ok);
  REQUIRE_THAT(result.message, ContainsSubstring("ExHiROM"));
}

TEST_CASE("ExHiROM rejects an image ending before its map-mode byte", "[unit][exhirom]") {
  SNES snes;
  const std::vector<uint8_t> rom(kExHiRomMapModeOffset, 0);
  CartProfile forced{};
  forced.mapper = MapperKind::kExHiROM;
  const BuildResult result = snes.LoadRomWithProfile(forced, rom);
  REQUIRE_FALSE(result.ok);
  REQUIRE_THAT(result.message, ContainsSubstring("too small for ExHiROM"));
}

TEST_CASE("ExHiROM tolerates absent SRAM metadata through automatic and explicit-profile loading", "[unit][exhirom]") {
  // Cover every truncation between the required map-mode byte and the
  // optional SRAM byte, plus the first size that includes the SRAM byte.
  for (std::size_t size = kExHiRomMapModeOffset + 1U; size <= kExHiRomSramSizeOffset + 1U; ++size) {
    const bool has_sram_byte = size > kExHiRomSramSizeOffset;
    std::vector<uint8_t> rom(size, 0);
    rom[kExHiRomMapModeOffset] = 0x25U;
    if (has_sram_byte) rom[kExHiRomSramSizeOffset] = 3U;

    for (const bool forced : {false, true}) {
      CAPTURE(size, forced);
      SNES snes;
      CartProfile profile{};
      profile.mapper = MapperKind::kExHiROM;
      const BuildResult result = forced ? snes.LoadRomWithProfile(profile, rom) : snes.LoadRom(rom);
      REQUIRE(result.ok);
      REQUIRE(snes.GetCartridge().GetMapperKind() == MapperKind::kExHiROM);
      REQUIRE(snes.GetCartridge().Size() == size);
      REQUIRE(snes.GetCartridge().SramSize() == (has_sram_byte ? 8192U : 0U));
      if (!forced) REQUIRE(result.profile.sram_bytes == snes.GetCartridge().SramSize());
    }
  }
}

// ---------------------------------------------------------------------------
// Load succeeds and Cartridge::GetMapperKind() reports kExHiROM
// ---------------------------------------------------------------------------

TEST_CASE("LoadRom: 8 MiB ExHiROM image loads and reports kExHiROM", "[unit][exhirom]") {
  SNES snes;
  const auto rom = MakeExHiRom(kExHiRom8M);
  const BuildResult result = snes.LoadRom(rom);
  REQUIRE(result.ok);
  REQUIRE(result.profile.mapper == MapperKind::kExHiROM);
  REQUIRE(snes.GetCartridge().GetMapperKind() == MapperKind::kExHiROM);
  REQUIRE(snes.GetCartridge().Size() == kExHiRom8M);
}

// ---------------------------------------------------------------------------
// Upper-bank ROM layout: $80-$BF pages $80-$FF and $C0-$FF pages $00-$FF
// see ROM[$000000..$3FFFFF]. Identical to HiROM's bigger-half mapping.
// ---------------------------------------------------------------------------

TEST_CASE("ExHiROM: bank $80 page $80 maps to ROM offset $008000", "[unit][exhirom]") {
  SNES snes;
  REQUIRE(snes.LoadRom(MakeExHiRom(kExHiRom8M)).ok);
  const auto& entry = GetEntry(snes, 0x80U, 0x80U);
  REQUIRE(entry.kind == PageDeviceKind::kMemory);
  // Upper-bank offset formula: (bank & 0x3F) << 16 | (page << 8) =
  // 0x00 << 16 | 0x80 << 8 = 0x008000.
  REQUIRE(entry.base_offset == 0x008000U);
}

TEST_CASE("ExHiROM: bank $C0 page $00 maps to ROM offset $000000", "[unit][exhirom]") {
  SNES snes;
  REQUIRE(snes.LoadRom(MakeExHiRom(kExHiRom8M)).ok);
  const auto& entry = GetEntry(snes, 0xC0U, 0x00U);
  REQUIRE(entry.kind == PageDeviceKind::kMemory);
  REQUIRE(entry.base_offset == 0x000000U);
}

// ---------------------------------------------------------------------------
// Lower-bank ROM layout: $00-$3F pages $80-$FF and $40-$7D pages $00-$FF
// see ROM[$400000..$7FFFFF]. This is the load-bearing ExHiROM signature.
// ---------------------------------------------------------------------------

TEST_CASE("ExHiROM: bank $40 page $00 maps to ROM offset $400000", "[unit][exhirom]") {
  SNES snes;
  REQUIRE(snes.LoadRom(MakeExHiRom(kExHiRom8M)).ok);
  const auto& entry = GetEntry(snes, 0x40U, 0x00U);
  REQUIRE(entry.kind == PageDeviceKind::kMemory);
  // Lower-bank base offset: 0x400000 + ((bank & 0x3F) << 16 | (page << 8))
  REQUIRE(entry.base_offset == 0x400000U);
}

TEST_CASE("ExHiROM: bank $00 page $80 (half-bank) maps to ROM offset $408000", "[unit][exhirom]") {
  SNES snes;
  REQUIRE(snes.LoadRom(MakeExHiRom(kExHiRom8M)).ok);
  const auto& entry = GetEntry(snes, 0x00U, 0x80U);
  REQUIRE(entry.kind == PageDeviceKind::kMemory);
  REQUIRE(entry.base_offset == 0x408000U);
}

TEST_CASE("ExHiROM: upper and lower banks see distinct ROM bytes", "[unit][exhirom]") {
  // The defining ExHiROM property: bank $00 page $80 (lower) and bank $80
  // page $80 (upper) point to *different* ROM regions. In HiROM these would
  // be identical FASTROM mirrors.
  SNES snes;
  REQUIRE(snes.LoadRom(MakeExHiRom(kExHiRom8M)).ok);
  const auto& lower = GetEntry(snes, 0x00U, 0x80U);
  const auto& upper = GetEntry(snes, 0x80U, 0x80U);
  REQUIRE(lower.base_offset != upper.base_offset);
  REQUIRE(upper.base_offset == 0x008000U);
  REQUIRE(lower.base_offset == 0x408000U);
}

// ---------------------------------------------------------------------------
// Smaller-half mirroring (6 MiB cart): lower banks $40-$5F cover the smaller
// 2 MiB half; banks $60-$7D wrap back to the start.
// ---------------------------------------------------------------------------

TEST_CASE("ExHiROM 6 MiB: bank $60 wraps to mirror bank $40", "[unit][exhirom]") {
  SNES snes;
  REQUIRE(snes.LoadRom(MakeExHiRom(kExHiRom6M)).ok);
  const auto& bank40 = GetEntry(snes, 0x40U, 0x00U);
  const auto& bank60 = GetEntry(snes, 0x60U, 0x00U);
  REQUIRE(bank40.base_offset == bank60.base_offset);
  REQUIRE(bank40.base_offset == 0x400000U);
}

// ---------------------------------------------------------------------------
// Bytes-through-the-bus check
// ---------------------------------------------------------------------------

TEST_CASE("ExHiROM: debug reads return the expected ROM bytes (upper banks)", "[unit][exhirom]") {
  SNES snes;
  REQUIRE(snes.LoadRom(MakeExHiRom(kExHiRom8M)).ok);

  // CPU $80:8123 → upper-bank ROM[$008123] in the test fixture.
  const auto result = snes.system_bus->DebugRead(0x80'8123U);
  REQUIRE(result.ok);
  const std::size_t expected_offset = 0x008123U;
  const uint8_t expected = static_cast<uint8_t>(((expected_offset >> 8) & 0xFFU) ^ 0x5AU);
  REQUIRE(result.value == expected);
}

TEST_CASE("ExHiROM: debug reads return the expected ROM bytes (lower banks)", "[unit][exhirom]") {
  SNES snes;
  REQUIRE(snes.LoadRom(MakeExHiRom(kExHiRom8M)).ok);

  // CPU $40:1234 → lower-bank ROM[$401234].
  const auto result = snes.system_bus->DebugRead(0x40'1234U);
  REQUIRE(result.ok);
  const std::size_t expected_offset = 0x401234U;
  const uint8_t expected = static_cast<uint8_t>(((expected_offset >> 8) & 0xFFU) ^ 0x5AU);
  REQUIRE(result.value == expected);
}

// ---------------------------------------------------------------------------
// FASTROM (MEMSEL) only affects upper banks
// ---------------------------------------------------------------------------

TEST_CASE("ExHiROM FastROM capability comes from its own header", "[unit][exhirom][fastrom]") {
  for (const bool fast : {false, true}) {
    CAPTURE(fast);
    SNES snes;
    auto rom = MakeExHiRom(kExHiRom6M);
    rom[kExHiRomMapModeOffset] = fast ? 0x35U : 0x25U;
    // Give the unused HiROM-header location the opposite FastROM bit.
    // Neither value is a valid HiROM mode, so detection remains unambiguous.
    rom[kHiRomMapModeOffset] = fast ? 0x00U : 0x10U;
    const BuildResult result = snes.LoadRom(rom);
    REQUIRE(result.ok);
    REQUIRE(result.profile.mapper == MapperKind::kExHiROM);
    REQUIRE(result.profile.fastrom_capable == fast);
    REQUIRE(snes.GetCartridge().IsFastRomCapable() == fast);
  }
}

TEST_CASE("ExHiROM: MEMSEL bit 0 retimes only $80-$FF", "[unit][exhirom]") {
  SNES snes;
  REQUIRE(snes.LoadRom(MakeExHiRom(kExHiRom8M)).ok);

  // After load, every ROM-bearing page is at 8 cycles regardless of bank.
  REQUIRE(GetEntry(snes, 0x40U, 0x00U).access_speed == 8U);
  REQUIRE(GetEntry(snes, 0x80U, 0x80U).access_speed == 8U);

  // Flip MEMSEL bit 0 to engage FASTROM.
  auto write = snes.system_bus->DebugWrite(0x00'420DU, 0x01U);
  REQUIRE(write.ok);

  // Upper banks retime to 6 cycles.
  REQUIRE(GetEntry(snes, 0x80U, 0x80U).access_speed == 6U);
  REQUIRE(GetEntry(snes, 0xC0U, 0x00U).access_speed == 6U);
  // Lower banks stay at 8 cycles (no FASTROM in $00-$7D).
  REQUIRE(GetEntry(snes, 0x40U, 0x00U).access_speed == 8U);
  REQUIRE(GetEntry(snes, 0x00U, 0x80U).access_speed == 8U);
}

// ---------------------------------------------------------------------------
// SRAM placement: defaults to HiROM-style ($20-$3F / $A0-$BF pages $60-$7F)
// ---------------------------------------------------------------------------

TEST_CASE("ExHiROM with SRAM: $A0:6000 lands on cart SRAM", "[unit][exhirom]") {
  SNES snes;
  REQUIRE(snes.LoadRom(MakeExHiRom(kExHiRom8M, 0x03U)).ok);  // 8 KiB SRAM
  REQUIRE(snes.GetCartridge().SramSize() == 8U * 1024U);

  // SRAM page should carry the tag bit so the slow-path knows it's SRAM,
  // not ROM. The exact tag is documented in cartridge.h as kSramOffsetTag.
  const auto& sram_entry = GetEntry(snes, 0xA0U, 0x60U);
  REQUIRE((sram_entry.base_offset & Cartridge::kSramOffsetTag) != 0U);
}

// ---------------------------------------------------------------------------
// Auto-detect: SNES::LoadRom routes a $25 map-mode image to ExHiROM
// ---------------------------------------------------------------------------

TEST_CASE("SNES::LoadRom: auto-detect routes $25 map mode to ExHiROM", "[unit][exhirom]") {
  SNES snes;
  const auto rom = MakeExHiRom(kExHiRom8M);
  const BuildResult result = snes.LoadRom(rom);
  REQUIRE(result.ok);
  REQUIRE(result.profile.mapper == MapperKind::kExHiROM);
  REQUIRE(snes.GetCartridge().GetMapperKind() == MapperKind::kExHiROM);
}
