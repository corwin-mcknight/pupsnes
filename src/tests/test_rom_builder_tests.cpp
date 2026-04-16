#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

std::vector<uint8_t> readBinaryFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());

    return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("Generated reset smoke test ROM has the expected reset vector and program bytes", "[unit]") {
    const std::filesystem::path rom_path = std::filesystem::path(PUPSNES_TEST_ROM_DIR) / "reset_smoke.sfc";
    const std::vector<uint8_t> rom = readBinaryFile(rom_path);

    REQUIRE(rom.size() == 32U * 1024U);
    REQUIRE(rom[0x0000] == 0xA9);
    REQUIRE(rom[0x0001] == 0x42);
    REQUIRE(rom[0x0002] == 0xEA);
    REQUIRE(rom[0x0003] == 0x80);
    REQUIRE(rom[0x0004] == 0xFE);
    REQUIRE(rom[0x7FFC] == 0x00);
    REQUIRE(rom[0x7FFD] == 0x80);
}

TEST_CASE("Generated WRAM signature ROM encodes long store into WRAM", "[unit]") {
    const std::filesystem::path rom_path = std::filesystem::path(PUPSNES_TEST_ROM_DIR) / "wram_signature.sfc";
    const std::vector<uint8_t> rom = readBinaryFile(rom_path);

    REQUIRE(rom.size() == 32U * 1024U);
    REQUIRE(rom[0x0000] == 0xA9);
    REQUIRE(rom[0x0001] == 0x5A);
    REQUIRE(rom[0x0002] == 0x8F);
    REQUIRE(rom[0x0003] == 0x00);
    REQUIRE(rom[0x0004] == 0x00);
    REQUIRE(rom[0x0005] == 0x7E);
    REQUIRE(rom[0x0006] == 0x80);
    REQUIRE(rom[0x0007] == 0xFE);
    REQUIRE(rom[0x7FFC] == 0x00);
    REQUIRE(rom[0x7FFD] == 0x80);
}

TEST_CASE("Generated instruction smoke ROM is assembled from shared boilerplate and code bytes", "[unit]") {
    const std::filesystem::path rom_path = std::filesystem::path(PUPSNES_TEST_ROM_DIR) / "instruction_smoke.sfc";
    const std::vector<uint8_t> rom = readBinaryFile(rom_path);

    REQUIRE(rom.size() == 32U * 1024U);
    REQUIRE(rom[0x0000] == 0xA9);
    REQUIRE(rom[0x0001] == 0x12);
    REQUIRE(rom[0x0002] == 0xEA);
    REQUIRE(rom[0x0003] == 0xA9);
    REQUIRE(rom[0x0004] == 0x34);
    REQUIRE(rom[0x0005] == 0xEA);
    REQUIRE(rom[0x0006] == 0x80);
    REQUIRE(rom[0x0007] == 0xFE);
    REQUIRE(rom[0x7FFC] == 0x00);
    REQUIRE(rom[0x7FFD] == 0x80);
}
