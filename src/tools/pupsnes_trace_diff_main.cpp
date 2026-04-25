#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include "pupsnes/debugger/trace_diff.h"

namespace {

struct Args {
  std::string pupsnes_path;
  std::string mesen_path;
  pupsnes::debugger::trace_diff::DiffOptions options;
  bool quiet = false;
};

void PrintUsage(std::ostream& out) {
  out << "usage: pupsnes-trace-diff --pupsnes <path> --mesen <path>\n"
      << "                         [--skip-pupsnes N] [--skip-mesen N]\n"
      << "                         [--align-on-first-pc]\n"
      << "                         [--max-mismatches N] [--quiet]\n";
}

bool ParseSize(std::string_view s, size_t& out) {
  out = 0;
  if (s.empty()) return false;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    out = out * 10 + static_cast<size_t>(c - '0');
  }
  return true;
}

bool ParseArgs(int argc, char** argv, Args& args, std::string& err) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto take_value = [&](std::string& dst) {
      if (i + 1 >= argc) {
        err = std::string(a) + " requires a value";
        return false;
      }
      dst = argv[++i];
      return true;
    };
    auto take_size = [&](size_t& dst) {
      if (i + 1 >= argc) {
        err = std::string(a) + " requires a value";
        return false;
      }
      if (!ParseSize(argv[++i], dst)) {
        err = std::string(a) + " expects a non-negative integer";
        return false;
      }
      return true;
    };
    if (a == "--pupsnes") {
      if (!take_value(args.pupsnes_path)) return false;
    } else if (a == "--mesen") {
      if (!take_value(args.mesen_path)) return false;
    } else if (a == "--skip-pupsnes") {
      if (!take_size(args.options.skip_pupsnes)) return false;
    } else if (a == "--skip-mesen") {
      if (!take_size(args.options.skip_mesen)) return false;
    } else if (a == "--align-on-first-pc") {
      args.options.align_on_first_pc = true;
    } else if (a == "--max-mismatches") {
      if (!take_size(args.options.max_mismatches)) return false;
    } else if (a == "--quiet") {
      args.quiet = true;
    } else if (a == "-h" || a == "--help") {
      PrintUsage(std::cout);
      std::exit(0);
    } else {
      err = "unknown argument: " + std::string(a);
      return false;
    }
  }
  if (args.pupsnes_path.empty() || args.mesen_path.empty()) {
    err = "--pupsnes and --mesen are both required";
    return false;
  }
  if (args.options.max_mismatches == 0) {
    err = "--max-mismatches must be >= 1";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  std::string err;
  if (!ParseArgs(argc, argv, args, err)) {
    std::cerr << "pupsnes-trace-diff: " << err << "\n";
    PrintUsage(std::cerr);
    return 2;
  }

  std::ifstream pup(args.pupsnes_path);
  if (!pup) {
    std::cerr << "pupsnes-trace-diff: cannot open pupsnes trace: "
              << args.pupsnes_path << "\n";
    return 2;
  }
  std::ifstream mes(args.mesen_path);
  if (!mes) {
    std::cerr << "pupsnes-trace-diff: cannot open mesen trace: "
              << args.mesen_path << "\n";
    return 2;
  }

  const auto result = pupsnes::debugger::trace_diff::Diff(pup, mes, args.options);
  if (!args.quiet) {
    pupsnes::debugger::trace_diff::FormatReport(result, std::cout);
  } else if (!result.mismatches.empty()) {
    pupsnes::debugger::trace_diff::FormatReport(result, std::cout);
  }

  if (!result.parse_error.empty()) return 2;
  if (!result.mismatches.empty()) return 1;
  return 0;
}
