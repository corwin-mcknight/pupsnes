// Headless PPU screenshot CLI. Loads a ROM, runs the emulator for N frame periods,
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
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pupsnes/core/emu_event.h"
#include "pupsnes/core/scheduler.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/debugger/console_event_sink.h"
#include "pupsnes/debugger/fan_out_emu_event_sink.h"
#include "pupsnes/debugger/file_event_sink.h"
#include "pupsnes/debugger/sha1.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/hw/rom/rom_format.h"
#include "pupsnes/hw/sppu/pixel_format.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/tools/trace_runner.h"  // for kMasterCyclesPerFrame

namespace {

constexpr std::string_view kUsage =
    "Usage: pupsnes-screenshot --rom <path> --frames N [--output <path>]\n"
    "                          [--log-events <cats>] [--log-events-file <path>]\n"
    "\n"
    "Loads a supported ROM image, boots a SNES, runs for N frame periods, then writes the\n"
    "PPU front buffer (logical width x height, BGR555 -> RGB888) to a binary\n"
    "PPM file. Default output is pupsnes-screenshot.ppm in the cwd.\n"
    "\n"
    "--log-events streams structured emulation events (BG mode changes, DMA\n"
    "bursts, NMI/IRQ, errors, ROM load) to stderr. <cats> is a comma list from\n"
    "ppu,dma,irq,error,host, or 'all'. --log-events-file writes the same lines to a\n"
    "file (header includes the ROM SHA-1); it defaults to all categories\n"
    "unless --log-events narrows them. Event logging is compiled out of\n"
    "PRODUCTION builds — these flags then produce no events.\n";

// Comma-separated category list ("ppu,dma" / "all") -> filter mask. Returns
// false on an unknown token.
[[nodiscard]] bool ParseCategoryMask(std::string_view list, uint32_t& mask_out) {
  uint32_t mask = 0;
  while (!list.empty()) {
    const std::size_t comma = list.find(',');
    const std::string_view token = list.substr(0, comma);
    if (token == "all") {
      mask = pupsnes::kAllEmuEventCategoriesMask;
    } else if (token == "ppu") {
      mask |= pupsnes::EmuEventCategoryBit(pupsnes::EmuEventCategory::kPpu);
    } else if (token == "dma") {
      mask |= pupsnes::EmuEventCategoryBit(pupsnes::EmuEventCategory::kDma);
    } else if (token == "irq") {
      mask |= pupsnes::EmuEventCategoryBit(pupsnes::EmuEventCategory::kInterrupt);
    } else if (token == "error") {
      mask |= pupsnes::EmuEventCategoryBit(pupsnes::EmuEventCategory::kError);
    } else if (token == "host") {
      mask |= pupsnes::EmuEventCategoryBit(pupsnes::EmuEventCategory::kHost);
    } else {
      return false;
    }
    if (comma == std::string_view::npos) break;
    list.remove_prefix(comma + 1);
  }
  mask_out = mask;
  return mask != 0;
}

[[nodiscard]] bool ParseUint(std::string_view s, uint64_t& out) {
  const char* begin = s.data();
  const char* end = begin + s.size();
  auto [ptr, ec] = std::from_chars(begin, end, out);
  return ec == std::errc{} && ptr == end;  // NOLINT(whitespace/braces)
}

[[nodiscard]] std::vector<uint8_t> ReadAllBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("cannot open ROM: " + path.string());
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
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
      const auto r = static_cast<uint8_t>(pupsnes::sppu::Expand5To8(bgr));
      const auto g = static_cast<uint8_t>(pupsnes::sppu::Expand5To8(static_cast<uint32_t>(bgr) >> 5U));
      const auto b = static_cast<uint8_t>(pupsnes::sppu::Expand5To8(static_cast<uint32_t>(bgr) >> 10U));
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
  uint32_t event_mask = 0;  // 0 = --log-events not given
  std::filesystem::path events_file_path;

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
    } else if (arg == "--log-events") {
      if (!ParseCategoryMask(require_value("--log-events"), event_mask)) {
        std::cerr << "pupsnes-screenshot: --log-events expects a comma list of ppu,dma,irq,error,host or 'all'\n";
        return 2;
      }
    } else if (arg == "--log-events-file") {
      events_file_path = std::string(require_value("--log-events-file"));
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

  // Event sinks must attach before LoadRom so the ROM_LOADED host event is
  // captured. Both sinks share the parsed mask; a bare --log-events-file
  // records every category.
  pupsnes::debugger::FanOutEmuEventSink event_fan_out;
  std::unique_ptr<pupsnes::debugger::ConsoleEventSink> console_events;
  std::unique_ptr<pupsnes::debugger::FileEventSink> file_events;
  if (event_mask != 0) {
    console_events = std::make_unique<pupsnes::debugger::ConsoleEventSink>(std::cerr);
    console_events->SetCategoryMask(event_mask);
    event_fan_out.Attach(console_events.get());
  }
  if (!events_file_path.empty()) {
    pupsnes::debugger::Sha1 sha1;
    sha1.Update(rom_bytes.data(), rom_bytes.size());
    file_events = std::make_unique<pupsnes::debugger::FileEventSink>(events_file_path.string(), sha1.Finalize());
    if (file_events->HasError()) {
      std::cerr << "pupsnes-screenshot: " << file_events->Error() << "\n";
      return 1;
    }
    if (event_mask != 0) {
      file_events->SetCategoryMask(event_mask);
    }
    event_fan_out.Attach(file_events.get());
  }
  if (event_fan_out.Count() > 0) {
    snes.SetEmuEventSink(&event_fan_out);
  }

  const pupsnes::BuildResult load_result = snes.LoadRom(rom_bytes);
  if (!load_result.ok) {
    std::cerr << "pupsnes-screenshot: ROM load failed: " << load_result.message << "\n";
    return 1;
  }
  snes.Reset();

  const pupsnes::TimeMasterT start_master = snes.GetMasterTime();
  const pupsnes::TimeMasterT cap =
      start_master + static_cast<pupsnes::TimeMasterT>(frames) * pupsnes::tools::kMasterCyclesPerFrame;

  if (auto err = pupsnes::tools::DriveMachineToMasterTime(snes, cap)) {
    std::cerr << "pupsnes-screenshot: " << *err << "\n";
    return 1;
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
