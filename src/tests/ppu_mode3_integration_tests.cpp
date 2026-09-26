#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "pupsnes/core/snes.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/tools/trace_runner.h"

namespace {

// Scene specification, independent of VRAM decoding and PPU color helpers.
// The ROM writes this indexed palette even when BG1 is using direct color.
uint16_t IndexedColor(uint32_t index) {
  if (index == 0U) return 0x1042;
  if (index >= 192U) {
    const uint32_t shade = 16U + index % 16U;
    switch ((index - 192U) / 16U) {
      case 0: return static_cast<uint16_t>(31U + 32U * shade);
      case 1: return static_cast<uint16_t>(shade + 1024U * 31U);
      case 2: return static_cast<uint16_t>(32U * 31U + 1024U * shade);
      default: return static_cast<uint16_t>(shade * 1057U);
    }
  }
  const uint32_t red = (index * 3U + 5U) % 32U;
  const uint32_t green = ((index / 8U) * 5U + 9U) % 32U;
  const uint32_t blue = ((index / 32U) * 7U + index * 3U + 13U) % 32U;
  return static_cast<uint16_t>(red + 32U * green + 1024U * blue);
}

uint16_t Bg1Color(uint32_t index, uint32_t palette, uint32_t y) {
  if (y < 80U) return IndexedColor(index);

  // Hardware direct-color layout: BBb00 GGGg0 RRRr0. Uppercase bits
  // come from the eight-bit texel, lowercase bits from the map palette.
  uint32_t red = 4U * (index % 8U) + 2U * (palette % 2U);
  uint32_t green = 4U * ((index / 8U) % 8U) + 2U * ((palette / 2U) % 2U);
  uint32_t blue = 8U * (index / 64U) + 4U * (palette / 4U);
  if (y >= 160U) {
    red = std::min(red + 4U, 31U);
    green = std::min(green + 8U, 31U);
    blue = std::min(blue + 12U, 31U);
  }
  return static_cast<uint16_t>(red + 32U * green + 1024U * blue);
}

uint16_t ExpectedPixel(uint32_t x, uint32_t y) {
  // BG1 consists of 16x16 motifs. Odd map columns/rows mirror the entire
  // motif, including its 8x8 subcharacters, not each subcharacter in place.
  const uint32_t tile_x = x / 16U;
  const uint32_t tile_y = y / 16U;
  const uint32_t u = tile_x % 2U != 0U ? 15U - x % 16U : x % 16U;
  const uint32_t v = tile_y % 2U != 0U ? 15U - y % 16U : y % 16U;
  const uint32_t variant = (tile_x / 4U + tile_y / 4U) % 2U;
  uint32_t bg1 = ((u % 8U) + 8U * (v % 8U) + 64U * (u / 8U) + 128U * (v / 8U)) ^ (variant * 85U);
  if ((u >= 4U && u < 8U && v >= 4U && v < 8U) || (u + 2U * v + variant) % 11U == 0U) bg1 = 0;
  const uint32_t bg1_palette = (tile_x + tile_y * 3U) % 8U;
  const bool bg1_high = (tile_x / 2U) % 2U != 0U;

  // BG2's perforated 8x8 pattern uses only CGRAM palettes 2 and 3,
  // including in the direct-color and color-math bands.
  const uint32_t bg2_x = (x / 8U) % 2U != 0U ? 7U - x % 8U : x % 8U;
  const uint32_t bg2_y = (y / 8U) % 2U != 0U ? 7U - y % 8U : y % 8U;
  const uint32_t bg2 = (bg2_x + bg2_y) % 4U == 0U ? 0U : 1U + (bg2_x / 2U + 3U * (bg2_y / 2U)) % 15U;
  const uint32_t bg2_palette = 2U + (x / 32U + y / 32U) % 2U;
  const bool bg2_high = (y / 16U) % 2U != 0U;

  // Twelve nonoverlapping 16x16 OBJs, four per band. The colors identify
  // priority 0/1/2/3 as yellow/magenta/cyan/white, respectively.
  const uint32_t priority = x / 64U;
  uint32_t obj = 0;
  if (x % 64U >= 24U && x % 64U < 40U && y % 80U >= 24U && y % 80U < 40U) {
    const uint32_t obj_x = x % 64U - 24U;
    const uint32_t obj_y = y % 80U - 24U;
    if (obj_x != 0U && obj_y != 0U && obj_x != 15U && obj_y != 15U && (obj_x + 2U * obj_y) % 9U != 0U) {
      obj = 1U + (obj_x / 4U + 4U * (obj_y / 4U)) % 15U;
    }
  }
  const uint16_t obj_color = IndexedColor(192U + priority * 16U + obj);
  const uint16_t bg2_color = IndexedColor(bg2_palette * 16U + bg2);

  // Highest-first hardware ladder. Test only opaque candidates: raw zero
  // must expose lower layers even with nonzero direct-color palette bits.
  if (obj != 0U && priority == 3U) return obj_color;
  if (bg1 != 0U && bg1_high) return Bg1Color(bg1, bg1_palette, y);
  if (obj != 0U && priority == 2U) return obj_color;
  if (bg2 != 0U && bg2_high) return bg2_color;
  if (obj != 0U && priority == 1U) return obj_color;
  if (bg1 != 0U) return Bg1Color(bg1, bg1_palette, y);
  if (obj != 0U && priority == 0U) return obj_color;
  if (bg2 != 0U) return bg2_color;
  return IndexedColor(0);
}

}  // namespace

TEST_CASE("Mode 3 ROM matches its layered indexed and direct-color scene across scheduler slices",
          "[unit][ppu][integration][mode3]") {
  std::ifstream input(std::filesystem::path(PUPSNES_TEST_ROM_DIR) / "ppu_mode3.sfc", std::ios::binary);
  REQUIRE(input.good());
  const std::vector<uint8_t> rom{std::istreambuf_iterator<char>(input), {}};
  constexpr pupsnes::TimeMasterT kTarget = 8U * 262U * 1364U;
  for (pupsnes::TimeMasterT slice : {pupsnes::TimeMasterT{379}, kTarget}) {
    CAPTURE(slice);
    pupsnes::SNES snes;
    REQUIRE(snes.LoadRom(rom).ok);
    snes.Reset();
    while (snes.GetMasterTime() < kTarget) {
      const pupsnes::TimeMasterT remaining = kTarget - snes.GetMasterTime();
      const auto error =
          pupsnes::tools::DriveMachineToMasterTime(snes, snes.GetMasterTime() + std::min(slice, remaining));
      INFO(error.value_or(""));
      REQUIRE_FALSE(error.has_value());
    }

    const auto view = snes.GetPpu().BuildFrontView();
    REQUIRE(view.width == 256U);
    REQUIRE(view.height == 224U);
    for (uint32_t y = 0; y < view.height; ++y) {
      for (uint32_t x = 0; x < view.width; ++x) {
        CAPTURE(x, y);
        REQUIRE(view.pixels[y * view.stride + x] == ExpectedPixel(x, y));
      }
    }
  }
}
