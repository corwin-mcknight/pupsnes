// Device lifecycle tests.
//
// Verifies the destroy-and-rebuild contract for Cartridge:
//   1. After LoadRomWithProfile, SNES::cartridge points at a fresh instance
//      with a NEW DeviceId (monotonic, no slot reuse).
//   2. SystemBus page-table entries created by the OLD cartridge's mapper
//      are scrubbed via SNES::DeregisterDevice → SystemBus::UnmapByDeviceId
//      when the old cart is destroyed.
//   3. Stable Device IDs (CPU, WRAM, MMIO, PPU, etc.) are preserved across
//      cart swaps — only cart-internal Devices churn.
//   4. SNES destruction does not crash even with the new deregistration
//      path: the destroying_ flag short-circuits Device::~Device's
//      callback so we don't poke a half-dead SystemBus.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/rom/cart_profile.h"
#include "pupsnes/hw/rom/cart_registry.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/core/device.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/memory/systembus.h"
#include "pupsnes/memory/wram.h"
#include "pupsnes/hw/rom/rom_format.h"
#include "systembus_test_access.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)

namespace {

std::vector<uint8_t> MakeValidLoRom() {
  std::vector<uint8_t> rom(32U * 1024U, 0xEAU);
  rom[kLoRomMapModeOffset] = 0x20U;
  rom[0x7FD6U] = 0x00U;
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
// New cart has a new DeviceId after LoadRomWithProfile (monotonic, no reuse)
// ---------------------------------------------------------------------------

TEST_CASE("LoadRomWithProfile: new cart receives a higher DeviceId than the previous cart",
          "[unit][device_lifecycle]") {
  SNES snes;
  const DeviceIdT initial_cart_id = snes.GetCartridge().GetDeviceId();
  const std::size_t initial_device_count = snes.GetDeviceCount();

  const auto rom = MakeValidLoRom();
  const auto profile = DetectCartProfile(rom).profile;
  const BuildResult result = snes.LoadRomWithProfile(profile, rom);
  REQUIRE(result.ok);

  // The new cart instance is what SNES::cartridge points at now.
  const DeviceIdT new_cart_id = snes.GetCartridge().GetDeviceId();
  REQUIRE(new_cart_id != initial_cart_id);
  // Monotonic: new ID is strictly greater (slots are nulled, never reused).
  REQUIRE(new_cart_id > initial_cart_id);
  // The OLD slot is now null — GetDevice returns nullptr for the old ID.
  REQUIRE(snes.GetDevice(initial_cart_id) == nullptr);
  // The new slot holds the new cart.
  REQUIRE(snes.GetDevice(new_cart_id) == &snes.GetCartridge());
  // Total device count grew by one (old slot kept as null, new slot pushed).
  REQUIRE(snes.GetDeviceCount() == initial_device_count + 1U);
}

// ---------------------------------------------------------------------------
// SystemBus page entries from the old cart are scrubbed
// ---------------------------------------------------------------------------

TEST_CASE("LoadRomWithProfile: HiROM-shaped slots cleared when swapping HiROM → LoROM",
          "[unit][device_lifecycle]") {
  SNES snes;

  // First: install a HiROM cart. Mapper writes SRAM at $20:60..$3F:7F.
  // Wait — vanilla HiROM SRAM is empty (sram_byte = 0). Use a non-empty
  // SRAM cart to populate $20:60.
  auto hirom = MakeValidHiRom();
  hirom[kHiRomSramSizeOffset] = 0x03U;  // 8 KiB SRAM
  const auto hirom_profile = DetectCartProfile(hirom).profile;
  REQUIRE(snes.LoadRomWithProfile(hirom_profile, hirom).ok);

  const DeviceIdT hirom_cart_id = snes.GetCartridge().GetDeviceId();
  const auto& sram_entry_before = SystemBusTestAccess::GetPageEntry(*snes.system_bus, 0x20U, 0x60U);
  REQUIRE(sram_entry_before.device_id == hirom_cart_id);
  REQUIRE(sram_entry_before.kind == PageDeviceKind::kMemory);

  // Now swap to a LoROM cart. LoROM doesn't map $20:60 (that slot is owned
  // by WRAM mirror or unmapped). The old HiROM SRAM entry there must be
  // scrubbed when the HiROM cart is destroyed.
  const auto lorom = MakeValidLoRom();
  const auto lorom_profile = DetectCartProfile(lorom).profile;
  REQUIRE(snes.LoadRomWithProfile(lorom_profile, lorom).ok);

  const auto& sram_entry_after = SystemBusTestAccess::GetPageEntry(*snes.system_bus, 0x20U, 0x60U);
  // The entry must NOT still claim the old HiROM cart's DeviceId.
  REQUIRE(sram_entry_after.device_id != hirom_cart_id);
}

// ---------------------------------------------------------------------------
// Stable Device IDs (CPU, WRAM, PPU, ...) survive cart swaps
// ---------------------------------------------------------------------------

TEST_CASE("LoadRomWithProfile: CPU / WRAM / PPU DeviceIds are stable across cart swaps",
          "[unit][device_lifecycle]") {
  SNES snes;
  const DeviceIdT cpu_id = snes.GetCpu().GetDeviceId();
  const DeviceIdT wram_id = snes.GetWram().GetDeviceId();
  const DeviceIdT ppu_id = snes.GetPpu().GetDeviceId();

  const auto rom = MakeValidLoRom();
  const auto profile = DetectCartProfile(rom).profile;
  REQUIRE(snes.LoadRomWithProfile(profile, rom).ok);

  REQUIRE(snes.GetCpu().GetDeviceId() == cpu_id);
  REQUIRE(snes.GetWram().GetDeviceId() == wram_id);
  REQUIRE(snes.GetPpu().GetDeviceId() == ppu_id);
  REQUIRE(snes.GetDevice(cpu_id) == static_cast<Device*>(&snes.GetCpu()));
  REQUIRE(snes.GetDevice(wram_id) == static_cast<Device*>(&snes.GetWram()));
  REQUIRE(snes.GetDevice(ppu_id) == static_cast<Device*>(&snes.GetPpu()));
}

// ---------------------------------------------------------------------------
// SNES destruction is safe — Device dtors don't crash
// ---------------------------------------------------------------------------

TEST_CASE("SNES destruction after multiple cart swaps does not crash", "[unit][device_lifecycle]") {
  // Build, then destroy a few SNES instances each with cart-swap churn.
  // If Device::~Device tries to call into a half-dead SystemBus during
  // SNES teardown the `destroying_` guard catches it; this test asserts
  // we never crash and the assertion count is at least non-zero so the
  // scope actually ran.
  for (int i = 0; i < 3; ++i) {
    SNES snes;
    const auto rom = MakeValidLoRom();
    const auto profile = DetectCartProfile(rom).profile;
    REQUIRE(snes.LoadRomWithProfile(profile, rom).ok);
    // SNES dtor runs at end of scope. Cartridge dtor calls DeregisterDevice
    // which now sees IsDestroying() == true and short-circuits.
  }
  SUCCEED("3 SNES instances built and destroyed without crashing");
}

// ---------------------------------------------------------------------------
// Failed build leaves the prior cart untouched
// ---------------------------------------------------------------------------

TEST_CASE("LoadRomWithProfile: failed build does not destroy the existing cart", "[unit][device_lifecycle]") {
  SNES snes;
  const auto rom = MakeValidLoRom();
  const auto profile = DetectCartProfile(rom).profile;
  REQUIRE(snes.LoadRomWithProfile(profile, rom).ok);
  const DeviceIdT cart_id_before = snes.GetCartridge().GetDeviceId();

  // Force a (LoROM, SA-1) profile — registry has no SA-1 builder so the
  // build fails. The existing cart must be untouched.
  CartProfile bad_profile = profile;
  bad_profile.coproc = Coprocessor::kSA1;
  const BuildResult result = snes.LoadRomWithProfile(bad_profile, rom);
  REQUIRE_FALSE(result.ok);
  REQUIRE(snes.GetCartridge().GetDeviceId() == cart_id_before);
}
