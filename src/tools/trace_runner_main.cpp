#include <charconv>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

#include "pupsnes/tools/trace_runner.h"

namespace {

constexpr std::string_view kUsage =
    "Usage: pupsnes-trace --rom <path> [--output <path>]\n"
    "                    (--instructions N | --master-cycles N | --frames N)\n"
    "\n"
    "Loads a supported ROM image, boots a SNES, and writes one trace line per\n"
    "retired instruction to <path> until the stop budget is satisfied.\n"
    "Default output is pupsnes-trace.log in the current directory.\n";

[[nodiscard]] bool ParseUint(std::string_view s, uint64_t& out) {
  const char* begin = s.data();
  const char* end = begin + s.size();
  auto [ptr, ec] = std::from_chars(begin, end, out);
  return ec == std::errc{} && ptr == end;
}

}  // namespace

int main(int argc, char** argv) {
  pupsnes::tools::TraceRunOptions opts;
  opts.output_path = "pupsnes-trace.log";

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    auto require_value = [&](const char* name) -> std::string_view {
      if (i + 1 >= argc) {
        std::cerr << "pupsnes-trace: missing value for " << name << "\n";
        std::cerr << kUsage;
        std::exit(2);
      }
      return argv[++i];
    };

    if (arg == "--rom") {
      opts.rom_path = std::string(require_value("--rom"));
    } else if (arg == "--output") {
      opts.output_path = std::string(require_value("--output"));
    } else if (arg == "--instructions") {
      uint64_t n = 0;
      if (!ParseUint(require_value("--instructions"), n)) {
        std::cerr << "pupsnes-trace: --instructions expects a positive integer\n";
        return 2;
      }
      opts.budget.instructions = n;
    } else if (arg == "--master-cycles") {
      uint64_t n = 0;
      if (!ParseUint(require_value("--master-cycles"), n)) {
        std::cerr << "pupsnes-trace: --master-cycles expects a positive integer\n";
        return 2;
      }
      opts.budget.master_cycles = static_cast<pupsnes::TimeMasterT>(n);
    } else if (arg == "--frames") {
      uint64_t n = 0;
      if (!ParseUint(require_value("--frames"), n)) {
        std::cerr << "pupsnes-trace: --frames expects a positive integer\n";
        return 2;
      }
      if (n > UINT32_MAX) {
        std::cerr << "pupsnes-trace: --frames too large\n";
        return 2;
      }
      opts.budget.frames = static_cast<uint32_t>(n);
    } else if (arg == "-h" || arg == "--help") {
      std::cout << kUsage;
      return 0;
    } else {
      std::cerr << "pupsnes-trace: unknown argument '" << arg << "'\n";
      std::cerr << kUsage;
      return 2;
    }
  }

  if (opts.rom_path.empty()) {
    std::cerr << "pupsnes-trace: --rom is required\n";
    std::cerr << kUsage;
    return 2;
  }

  const auto result = pupsnes::tools::RunTrace(opts);
  if (!result.ok) {
    std::cerr << "pupsnes-trace: " << result.error << "\n";
    return 1;
  }

  std::cerr << "pupsnes-trace: wrote " << result.instructions_emitted << " instructions (" << result.master_time_elapsed
            << " master cycles) to " << opts.output_path.string() << "\n";
  return 0;
}
