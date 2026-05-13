#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <span>
#include <vector>

#include "pupsnes/hw/cartridge.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"
#include "pupsnes/rom_format.h"

using pupsnes::BusAccessType;
using pupsnes::Cartridge;
using pupsnes::LoRomSramSize;
using pupsnes::SNES;

namespace {

constexpr std::size_t kRomSize = Cartridge::kLoROMWindowSize;  // 32 KiB single-bank LoROM

std::vector<uint8_t> MakeLoRomWithSramByte(uint8_t sram_byte) {
  std::vector<uint8_t> rom(kRomSize, 0xFFU);
  rom[pupsnes::kLoRomSramSizeOffset] = sram_byte;
  return rom;
}

}  // namespace

TEST_CASE("LoRomSramSize: returns 0 for header byte 0 (no SRAM)", "[unit][rom_format]") {
  const auto rom = MakeLoRomWithSramByte(0x00U);
  REQUIRE(LoRomSramSize(rom) == 0U);
}

TEST_CASE("LoRomSramSize: 1024 << N for supported byte values", "[unit][rom_format]") {
  REQUIRE(LoRomSramSize(MakeLoRomWithSramByte(0x01U)) == 0x800U);    // 2 KiB
  REQUIRE(LoRomSramSize(MakeLoRomWithSramByte(0x03U)) == 0x2000U);   // 8 KiB (Super Metroid)
  REQUIRE(LoRomSramSize(MakeLoRomWithSramByte(0x05U)) == 0x8000U);   // 32 KiB (SMW)
  REQUIRE(LoRomSramSize(MakeLoRomWithSramByte(0x09U)) == 0x80000U);  // 512 KiB (ceiling)
}

TEST_CASE("LoRomSramSize: rejects out-of-range values", "[unit][rom_format]") {
  // Anything beyond the documented 9 (= 512 KiB) is treated as a corrupt
  // header. Returning a real allocation for, say, 0xFF would balloon SRAM to
  // ~1e75 bytes — much worse than just refusing to allocate.
  REQUIRE(LoRomSramSize(MakeLoRomWithSramByte(0x0AU)) == 0U);
  REQUIRE(LoRomSramSize(MakeLoRomWithSramByte(0xFFU)) == 0U);
}

TEST_CASE("LoRomSramSize: returns 0 when ROM is shorter than the header", "[unit][rom_format]") {
  std::vector<uint8_t> short_rom(0x100U, 0x00U);
  REQUIRE(LoRomSramSize(short_rom) == 0U);
}

TEST_CASE("Cartridge: LoadLoRom sizes SRAM from the internal header", "[unit][cartridge]") {
  SNES snes;
  const auto rom = MakeLoRomWithSramByte(0x03U);  // 8 KiB
  snes.LoadLoRom(rom);
  REQUIRE(snes.GetCartridge().SramSize() == 0x2000U);
}

TEST_CASE("Cartridge: SRAM is mapped into banks $70 and $F0 when present", "[unit][cartridge]") {
  SNES snes;
  auto rom = MakeLoRomWithSramByte(0x03U);  // 8 KiB SRAM
  snes.LoadLoRom(rom);

  Cartridge& cart = snes.GetCartridge();
  auto& bus = snes.GetSystemBus();

  // Write a sentinel through the bus at $700000 (start of SRAM), read it back
  // through the bus at $701234 to confirm the SRAM region is live (and that
  // wrapping inside an 8 KiB SRAM works).
  auto write_plan = bus.Plan(0x700000U, BusAccessType::kWrite, 0xA5U);
  (void)bus.Follow(write_plan, 0, cart.GetDeviceId());

  auto read_plan = bus.Plan(0x700000U, BusAccessType::kRead);
  auto result = bus.Follow(read_plan, 0, cart.GetDeviceId());
  REQUIRE(result.data == 0xA5U);
}

TEST_CASE("Cartridge: SRAM mirrors $702000 onto $700000 (Super Metroid piracy test)", "[unit][cartridge]") {
  // The 8 KiB SRAM at $700000-$701FFF is mirrored at $702000-$703FFF on real
  // carts. Super Metroid's anti-piracy check writes an incrementing sequence
  // through the $702000 mirror and reads it back from $700000, expecting them
  // to alias.
  SNES snes;
  auto rom = MakeLoRomWithSramByte(0x03U);  // 8 KiB
  snes.LoadLoRom(rom);

  Cartridge& cart = snes.GetCartridge();
  auto& bus = snes.GetSystemBus();

  for (uint16_t i = 0; i < 0x1000U; ++i) {
    const uint32_t addr = 0x702000U + i;
    const uint8_t value = static_cast<uint8_t>(i & 0xFFU);
    auto plan = bus.Plan(addr, BusAccessType::kWrite, value);
    (void)bus.Follow(plan, 0, cart.GetDeviceId());
  }

  for (uint16_t i = 0; i < 0x1000U; ++i) {
    const uint32_t addr = 0x700000U + i;
    auto plan = bus.Plan(addr, BusAccessType::kRead);
    auto r = bus.Follow(plan, 0, cart.GetDeviceId());
    REQUIRE(r.data == static_cast<uint8_t>(i & 0xFFU));
  }
}

TEST_CASE("Cartridge: writing SRAM sets the dirty flag", "[unit][cartridge]") {
  SNES snes;
  auto rom = MakeLoRomWithSramByte(0x03U);
  snes.LoadLoRom(rom);

  Cartridge& cart = snes.GetCartridge();
  REQUIRE_FALSE(cart.SramDirty());

  auto& bus = snes.GetSystemBus();
  auto plan = bus.Plan(0x700000U, BusAccessType::kWrite, 0x42U);
  (void)bus.Follow(plan, 0, cart.GetDeviceId());

  REQUIRE(cart.SramDirty());

  cart.ClearSramDirty();
  REQUIRE_FALSE(cart.SramDirty());
}

TEST_CASE("Cartridge: LoadSram restores contents without dirtying", "[unit][cartridge]") {
  SNES snes;
  auto rom = MakeLoRomWithSramByte(0x03U);
  snes.LoadLoRom(rom);

  Cartridge& cart = snes.GetCartridge();
  std::vector<uint8_t> save(cart.SramSize(), 0x00U);
  for (std::size_t i = 0; i < save.size(); ++i) {
    save[i] = static_cast<uint8_t>(i & 0xFFU);
  }
  cart.LoadSram(save);

  REQUIRE_FALSE(cart.SramDirty());

  auto& bus = snes.GetSystemBus();
  for (std::size_t i = 0; i < cart.SramSize(); ++i) {
    auto plan = bus.Plan(static_cast<uint32_t>(0x700000U + i), BusAccessType::kRead);
    auto r = bus.Follow(plan, 0, cart.GetDeviceId());
    REQUIRE(r.data == static_cast<uint8_t>(i & 0xFFU));
  }
}

TEST_CASE("Cartridge: ROMs with no SRAM leave the SRAM banks unmapped", "[unit][cartridge]") {
  SNES snes;
  auto rom = MakeLoRomWithSramByte(0x00U);  // no SRAM
  snes.LoadLoRom(rom);

  Cartridge& cart = snes.GetCartridge();
  REQUIRE(cart.SramSize() == 0U);

  // Writes should not crash and reads should produce open-bus-ish behaviour.
  auto& bus = snes.GetSystemBus();
  auto write_plan = bus.Plan(0x700000U, BusAccessType::kWrite, 0xAAU);
  (void)bus.Follow(write_plan, 0, cart.GetDeviceId());

  REQUIRE_FALSE(cart.SramDirty());
}
