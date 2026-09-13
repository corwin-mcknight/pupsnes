#include <unistd.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "pupsnes/core/snes.h"
#include "pupsnes/debugger/sram_persistence.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/hw/rom/rom_format.h"
#include "pupsnes/memory/systembus.h"

namespace {

struct SaveFixture {
  pupsnes::SNES snes;
  pupsnes::debugger::SramPersistence persistence;
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("pupsnes_sram_" + std::to_string(::getpid()) + "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
  std::filesystem::path rom_path = directory / "game.sfc";
  std::filesystem::path save_path = directory / "game.srm";

  SaveFixture() {
    std::filesystem::create_directory(directory);
    ReloadRom();
  }

  ~SaveFixture() {
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
  }

  void ReloadRom(uint8_t sram_byte = 3) {
    std::vector<uint8_t> rom(pupsnes::Cartridge::kLoROMWindowSize, 0xFFU);
    rom[pupsnes::kLoRomSramSizeOffset] = sram_byte;
    REQUIRE(snes.LoadRom(rom).ok);
  }

  void Write(uint8_t value) { REQUIRE(snes.GetSystemBus().DebugWrite(0x700000, value).ok); }

  void SeedSave() {
    std::ofstream stream(save_path, std::ios::binary);
    stream << "existing save";
  }

  std::string ReadSave() const {
    std::ifstream stream(save_path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  }
};

}  // namespace

TEST_CASE("SRAM persistence restores a save after cartridge replacement", "[unit][debugger][sram_persistence]") {
  SaveFixture f;
  REQUIRE(f.persistence.Load(f.snes.GetCartridge(), f.rom_path, true).empty());
  REQUIRE(f.persistence.Flush(f.snes.GetCartridge(), true).empty());
  REQUIRE_FALSE(std::filesystem::exists(f.save_path));
  f.Write(0x42);
  REQUIRE(f.persistence.Flush(f.snes.GetCartridge(), true).empty());
  REQUIRE_FALSE(f.snes.GetCartridge().SramDirty());
  REQUIRE(std::filesystem::file_size(f.save_path) == 8192);

  f.ReloadRom();
  pupsnes::debugger::SramPersistence reopened;
  REQUIRE(reopened.Load(f.snes.GetCartridge(), f.rom_path, true).empty());
  REQUIRE(f.snes.GetCartridge().SramView()[0] == 0x42);
  REQUIRE_FALSE(f.snes.GetCartridge().SramDirty());
  f.snes.Reset();
  REQUIRE(f.snes.GetCartridge().SramView()[0] == 0x42);
}

TEST_CASE("Disabled SRAM persistence leaves disk saves untouched", "[unit][debugger][sram_persistence]") {
  SaveFixture f;
  f.SeedSave();
  REQUIRE(f.persistence.Load(f.snes.GetCartridge(), f.rom_path, false).empty());
  REQUIRE(f.snes.GetCartridge().SramView()[0] == 0xFF);
  f.Write(0x42);
  REQUIRE(f.persistence.Flush(f.snes.GetCartridge(), false).empty());
  REQUIRE(f.ReadSave() == "existing save");
  REQUIRE(f.snes.GetCartridge().SramDirty());

  // Enabling mid-session saves the current SRAM without reloading old data.
  REQUIRE(f.persistence.Flush(f.snes.GetCartridge(), true).empty());
  REQUIRE(f.ReadSave()[0] == 0x42);
  f.Write(0x24);
  REQUIRE(f.persistence.Flush(f.snes.GetCartridge(), false).empty());
  REQUIRE(f.ReadSave()[0] == 0x42);
  REQUIRE(f.snes.GetCartridge().SramView()[0] == 0x24);
}

TEST_CASE("SRAM persistence keeps each ROM's save separate", "[unit][debugger][sram_persistence]") {
  SaveFixture f;
  REQUIRE(f.persistence.Load(f.snes.GetCartridge(), f.rom_path, true).empty());
  f.Write(0x42);
  REQUIRE(f.persistence.Flush(f.snes.GetCartridge(), true).empty());
  f.ReloadRom();
  REQUIRE(f.persistence.Load(f.snes.GetCartridge(), f.directory / "second.smc", true).empty());
  f.Write(0x24);
  REQUIRE(f.persistence.Flush(f.snes.GetCartridge(), true).empty());
  REQUIRE(f.ReadSave()[0] == 0x42);
  f.ReloadRom();
  REQUIRE(f.persistence.Load(f.snes.GetCartridge(), f.directory / "second.smc", true).empty());
  REQUIRE(f.snes.GetCartridge().SramView()[0] == 0x24);
}

TEST_CASE("Failed SRAM writes preserve the old save and remain retryable", "[unit][debugger][sram_persistence]") {
  SaveFixture f;
  f.SeedSave();
  REQUIRE(f.persistence.Load(f.snes.GetCartridge(), f.rom_path, true).empty());
  REQUIRE(f.snes.GetCartridge().SramView()[13] == 0xFF);  // Short saves are padded.
  f.Write(0x42);
  const auto temporary = f.directory / "game.srm.tmp";
  std::filesystem::create_directory(temporary);
  REQUIRE_FALSE(f.persistence.Flush(f.snes.GetCartridge(), true).empty());
  REQUIRE(f.ReadSave() == "existing save");
  REQUIRE(f.snes.GetCartridge().SramDirty());
  std::filesystem::remove(temporary);
  REQUIRE(f.persistence.Flush(f.snes.GetCartridge(), true).empty());
  REQUIRE(f.ReadSave()[0] == 0x42);
  REQUIRE_FALSE(f.snes.GetCartridge().SramDirty());
  REQUIRE_FALSE(std::filesystem::exists(temporary));
}

TEST_CASE("Unreadable SRAM saves cannot be overwritten by fresh session data", "[unit][debugger][sram_persistence]") {
  SaveFixture f;
  std::filesystem::create_directory(f.save_path);
  REQUIRE_FALSE(f.persistence.Load(f.snes.GetCartridge(), f.rom_path, true).empty());
  f.Write(0x42);
  std::filesystem::remove(f.save_path);
  f.SeedSave();
  REQUIRE_FALSE(f.persistence.Flush(f.snes.GetCartridge(), true).empty());
  REQUIRE(f.ReadSave() == "existing save");
  REQUIRE(f.snes.GetCartridge().SramDirty());
}

TEST_CASE("Cartridges without SRAM never access save files", "[unit][debugger][sram_persistence]") {
  SaveFixture f;
  f.ReloadRom(0);
  std::filesystem::create_directory(f.save_path);
  REQUIRE(f.persistence.Load(f.snes.GetCartridge(), f.rom_path, true).empty());
  REQUIRE(f.persistence.Flush(f.snes.GetCartridge(), true).empty());
  REQUIRE(std::filesystem::is_directory(f.save_path));
}

TEST_CASE("Clearing SRAM preserves bus mappings and respects persistence", "[unit][debugger][sram_persistence]") {
  SaveFixture f;
  f.SeedSave();
  REQUIRE(f.persistence.Load(f.snes.GetCartridge(), f.rom_path, true).empty());
  auto& cart = f.snes.GetCartridge();
  const auto* mapped_data = cart.SramData();
  cart.ClearSram();
  REQUIRE(cart.SramData() == mapped_data);
  REQUIRE(cart.SramDirty());
  REQUIRE(std::ranges::all_of(cart.SramView(), [](uint8_t byte) { return byte == 0xFF; }));
  const auto read = f.snes.GetSystemBus().DebugRead(0x700000);
  REQUIRE(read.ok);
  REQUIRE(read.value == 0xFF);

  SECTION("Disabled persistence keeps the original disk save") {
    REQUIRE(f.persistence.Flush(cart, false).empty());
    REQUIRE(f.ReadSave() == "existing save");
    REQUIRE(cart.SramDirty());
  }
  SECTION("Enabled persistence restores empty SRAM on reload") {
    REQUIRE(f.persistence.Flush(cart, true).empty());
    REQUIRE_FALSE(cart.SramDirty());
    f.ReloadRom();
    REQUIRE(f.persistence.Load(f.snes.GetCartridge(), f.rom_path, true).empty());
    REQUIRE(std::ranges::all_of(f.snes.GetCartridge().SramView(), [](uint8_t byte) { return byte == 0xFF; }));
  }
}

TEST_CASE("Clearing a cartridge without SRAM is a no-op", "[unit][debugger][sram_persistence]") {
  SaveFixture f;
  f.ReloadRom(0);
  f.snes.GetCartridge().ClearSram();
  REQUIRE(f.snes.GetCartridge().SramView().empty());
  REQUIRE_FALSE(f.snes.GetCartridge().SramDirty());
}
