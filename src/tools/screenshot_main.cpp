// Headless PPU screenshot CLI. Loads a LoROM, runs the emulator for N frames,
// and writes the PPU's front-buffer view to a binary PPM (P6, 8-bit per channel).
//
// Used by external tools to take a visual snapshot of a ROM's PPU output
// without bringing up the ImGui debugger. Mirrors pupsnes-trace's run loop.

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/core/scheduler.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/hw/rom/rom_format.h"
#include "pupsnes/tools/trace_runner.h"  // for kMasterCyclesPerFrame

namespace {

constexpr std::string_view kUsage =
    "Usage: pupsnes-screenshot --rom <path> --frames N [--output <path>]\n"
    "\n"
    "Loads a LoROM image, boots a SNES, runs for N frames, then writes the\n"
    "PPU front buffer (logical width x height, BGR555 -> RGB888) to a binary\n"
    "PPM file. Default output is pupsnes-screenshot.ppm in the cwd.\n";

[[nodiscard]] bool ParseUint(std::string_view s, uint64_t& out) {
  const char* begin = s.data();
  const char* end = begin + s.size();
  auto [ptr, ec] = std::from_chars(begin, end, out);
  return ec == std::errc{} && ptr == end;
}

[[nodiscard]] std::vector<uint8_t> ReadAllBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("cannot open ROM: " + path.string());
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Expand a BGR555 5:5:5 channel to 8 bits by replicating high bits into low.
[[nodiscard]] constexpr uint8_t Expand5To8(uint32_t c5) {
  const uint32_t c = c5 & 0x1FU;
  return static_cast<uint8_t>((c << 3U) | (c >> 2U));
}

void WritePpm(const std::filesystem::path& path, const pupsnes::FrameBufferView& view) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("cannot open output: " + path.string());
  }
  out << "P6\n" << view.width << " " << view.height << "\n255\n";

  std::vector<uint8_t> row(static_cast<std::size_t>(view.width) * 3U);
  for (uint32_t y = 0; y < view.height; ++y) {
    const uint16_t* src = view.pixels + static_cast<std::size_t>(y) * view.stride;
    for (uint32_t x = 0; x < view.width; ++x) {
      const uint16_t bgr = src[x];
      const uint8_t r = Expand5To8(bgr);
      const uint8_t g = Expand5To8(static_cast<uint32_t>(bgr) >> 5U);
      const uint8_t b = Expand5To8(static_cast<uint32_t>(bgr) >> 10U);
      row[static_cast<std::size_t>(x) * 3U + 0U] = r;
      row[static_cast<std::size_t>(x) * 3U + 1U] = g;
      row[static_cast<std::size_t>(x) * 3U + 2U] = b;
    }
    out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path rom_path;
  std::filesystem::path output_path = "pupsnes-screenshot.ppm";
  uint64_t frames = 0;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    auto require_value = [&](const char* name) -> std::string_view {
      if (i + 1 >= argc) {
        std::cerr << "pupsnes-screenshot: missing value for " << name << "\n";
        std::cerr << kUsage;
        std::exit(2);
      }
      return argv[++i];
    };

    if (arg == "--rom") {
      rom_path = std::string(require_value("--rom"));
    } else if (arg == "--output") {
      output_path = std::string(require_value("--output"));
    } else if (arg == "--frames") {
      if (!ParseUint(require_value("--frames"), frames)) {
        std::cerr << "pupsnes-screenshot: --frames expects a positive integer\n";
        return 2;
      }
    } else if (arg == "-h" || arg == "--help") {
      std::cout << kUsage;
      return 0;
    } else {
      std::cerr << "pupsnes-screenshot: unknown argument '" << arg << "'\n";
      std::cerr << kUsage;
      return 2;
    }
  }

  if (rom_path.empty()) {
    std::cerr << "pupsnes-screenshot: --rom is required\n";
    std::cerr << kUsage;
    return 2;
  }
  if (frames == 0) {
    std::cerr << "pupsnes-screenshot: --frames is required and must be > 0\n";
    std::cerr << kUsage;
    return 2;
  }

  std::vector<uint8_t> rom_bytes;
  try {
    rom_bytes = ReadAllBytes(rom_path);
  } catch (const std::exception& ex) {
    std::cerr << "pupsnes-screenshot: " << ex.what() << "\n";
    return 1;
  }
  pupsnes::StripSmcCopierHeader(rom_bytes);

  pupsnes::SNES snes;
  const pupsnes::RomLoadResult load_result = snes.LoadRom(rom_bytes);
  if (!load_result.ok) {
    std::cerr << "pupsnes-screenshot: ROM load failed: " << load_result.message << "\n";
    return 1;
  }
  snes.Reset();

  const pupsnes::TimeMasterT start_master = snes.GetMasterTime();
  const pupsnes::TimeMasterT cap =
      start_master + static_cast<pupsnes::TimeMasterT>(frames) * pupsnes::tools::kMasterCyclesPerFrame;

  // Defensive: bail out if the CPU stops making forward progress (STP / halt).
  constexpr int kMaxStuckIterations = 16;
  int stuck_iterations = 0;
  pupsnes::TimeMasterT last_master = start_master;

  while (true) {
    const pupsnes::TimeMasterT now_master = snes.GetMasterTime();
    if (now_master >= cap) break;

    pupsnes::TimeMasterT target = snes.GetScheduler().NextEventMasterTime();
    if (cap < target) target = cap;

    try {
      static_cast<void>(snes.GetCpu().TickToTarget(target));
    } catch (const std::exception& ex) {
      std::cerr << "pupsnes-screenshot: CPU exception: " << ex.what() << "\n";
      return 1;
    }
    const pupsnes::TimeMasterT after_tick = snes.GetMasterTime();
    snes.MachineSync(after_tick);
    snes.GetScheduler().FireEventsThrough(after_tick);

    if (after_tick == last_master) {
      if (++stuck_iterations >= kMaxStuckIterations) {
        std::cerr << "pupsnes-screenshot: CPU made no forward progress "
                     "(STP/halt or tick budget too small)\n";
        return 1;
      }
    } else {
      stuck_iterations = 0;
      last_master = after_tick;
    }
  }

  const pupsnes::FrameBufferView view = snes.GetPpu().BuildFrontView();
  try {
    WritePpm(output_path, view);
  } catch (const std::exception& ex) {
    std::cerr << "pupsnes-screenshot: " << ex.what() << "\n";
    return 1;
  }

  std::cerr << "pupsnes-screenshot: wrote " << view.width << "x" << view.height << " PPM to " << output_path.string()
            << " after " << frames << " frames (" << (snes.GetMasterTime() - start_master) << " master cycles)\n";
  return 0;
}
