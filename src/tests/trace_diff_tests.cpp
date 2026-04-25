#include <catch2/catch_test_macros.hpp>
#include <sstream>
#include <string>

#include "pupsnes/debugger/trace_diff.h"

TEST_CASE("trace_diff library version is 1", "[unit][trace_diff]") {
  REQUIRE(pupsnes::debugger::trace_diff::kLibraryVersion == 1U);
}

TEST_CASE("CpuState equality ignores hi_byte_valid flags", "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::CpuFlags;
  using pupsnes::debugger::trace_diff::CpuState;

  CpuState a{};
  a.pb = 0x00;
  a.pc = 0x8000;
  a.A = 0x0094;
  a.X = 0x0010;
  a.Y = 0x0004;
  a.S = 0x01FF;
  a.D = 0x0000;
  a.DB = 0x00;
  a.flags = CpuFlags{false, false, true, true, false, true, false, false};
  a.a_hi_valid = true;
  a.x_hi_valid = true;
  a.y_hi_valid = true;

  CpuState b = a;
  REQUIRE(a == b);

  b.X = 0x0011;
  REQUIRE_FALSE(a == b);
}

TEST_CASE("ParsePFlags decodes uppercase as set and lowercase as clear",
          "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::CpuFlags;
  using pupsnes::debugger::trace_diff::ParsePFlags;

  auto all_clear = ParsePFlags("nvmxdizc");
  REQUIRE(all_clear.has_value());
  REQUIRE(*all_clear == CpuFlags{});

  auto mesen_example = ParsePFlags("nvMXdIZc");
  REQUIRE(mesen_example.has_value());
  CpuFlags expected{};
  expected.m = true;
  expected.x = true;
  expected.i = true;
  expected.z = true;
  REQUIRE(*mesen_example == expected);
}

TEST_CASE("ParsePFlags rejects wrong length or wrong letters",
          "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::ParsePFlags;

  REQUIRE_FALSE(ParsePFlags("nvmxdiz").has_value());
  REQUIRE_FALSE(ParsePFlags("nvmxdizcc").has_value());
  REQUIRE_FALSE(ParsePFlags("nvpbhiZc").has_value());
}

TEST_CASE("ClassifyMesenLine identifies CPU and APU lines", "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::ClassifyMesenLine;
  using pupsnes::debugger::trace_diff::MesenLineKind;

  constexpr const char* kCpuLine =
      "008000  SEI                            "
      "A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc "
      "V:0   H:48  Fr:237 Cycle:1 BC:78";
  constexpr const char* kApuLine =
      "FFC5  MOV (X),A [$00EF] = $00          "
      "A:00 X:EF Y:00 S:EF P:nvpbhiZc V:0   H:48";

  REQUIRE(ClassifyMesenLine(kCpuLine) == MesenLineKind::kCpu);
  REQUIRE(ClassifyMesenLine(kApuLine) == MesenLineKind::kSkipped);
  REQUIRE(ClassifyMesenLine("") == MesenLineKind::kSkipped);
  REQUIRE(ClassifyMesenLine("   ") == MesenLineKind::kSkipped);
  REQUIRE(ClassifyMesenLine("not a trace line at all") == MesenLineKind::kSkipped);
  REQUIRE(ClassifyMesenLine("008000 SEI junk") == MesenLineKind::kSkipped);
}

TEST_CASE("ParseMesenCpuLine extracts 16-bit regs and flags",
          "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::CpuFlags;
  using pupsnes::debugger::trace_diff::ParseMesenCpuLine;

  constexpr const char* kLine =
      "008000  SEI                            "
      "A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc "
      "V:0   H:48  Fr:237 Cycle:1 BC:78";

  auto result = ParseMesenCpuLine(kLine);
  REQUIRE(result.has_value());
  const auto& s = *result;
  REQUIRE(s.pb == 0x00);
  REQUIRE(s.pc == 0x8000);
  REQUIRE(s.A == 0x9400);
  REQUIRE(s.X == 0x0010);
  REQUIRE(s.Y == 0x0004);
  REQUIRE(s.S == 0x01FF);
  REQUIRE(s.D == 0x0000);
  REQUIRE(s.DB == 0x00);
  REQUIRE(s.a_hi_valid);
  REQUIRE(s.x_hi_valid);
  REQUIRE(s.y_hi_valid);
  CpuFlags expected{};
  expected.m = true;
  expected.x = true;
  expected.i = true;
  expected.z = true;
  REQUIRE(s.flags == expected);
}

TEST_CASE("ParseMesenCpuLine handles 8-bit A with 16-bit X/Y",
          "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::ParseMesenCpuLine;

  constexpr const char* kLine =
      "00802A  STA $7F8000 = $F0A9            "
      "A:A9 X:017D Y:03FD S:01FF D:0000 DB:00 P:NvMxdIzC "
      "V:0   H:163 Fr:237 Cycle:57 BC:8F 00 80 7F";

  auto result = ParseMesenCpuLine(kLine);
  REQUIRE(result.has_value());
  const auto& s = *result;
  REQUIRE(s.A == 0x00A9);
  REQUIRE_FALSE(s.a_hi_valid);
  REQUIRE(s.X == 0x017D);
  REQUIRE(s.x_hi_valid);
  REQUIRE(s.Y == 0x03FD);
  REQUIRE(s.y_hi_valid);
}

TEST_CASE("ParseMesenCpuLine handles all-8-bit mode", "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::ParseMesenCpuLine;

  constexpr const char* kLine =
      "00FF00  LDA #$12                       "
      "A:12 X:34 Y:56 S:01FF D:0000 DB:00 P:nvMXdIzc "
      "V:0   H:4   Fr:0 Cycle:2 BC:A9 12";

  auto result = ParseMesenCpuLine(kLine);
  REQUIRE(result.has_value());
  const auto& s = *result;
  REQUIRE(s.A == 0x0012);
  REQUIRE(s.X == 0x0034);
  REQUIRE(s.Y == 0x0056);
  REQUIRE_FALSE(s.a_hi_valid);
  REQUIRE_FALSE(s.x_hi_valid);
  REQUIRE_FALSE(s.y_hi_valid);
}

TEST_CASE("ParseMesenCpuLine rejects missing columns", "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::ParseMesenCpuLine;

  REQUIRE_FALSE(ParseMesenCpuLine(
      "008000  SEI  A:9400 X:0010 Y:0004 S:01FF D:0000 "
      "P:nvMXdIZc Fr:0 Cycle:1 BC:78").has_value());

  REQUIRE_FALSE(ParseMesenCpuLine(
      "008000  SEI  A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 "
      "Fr:0 Cycle:1 BC:78").has_value());

  REQUIRE_FALSE(ParseMesenCpuLine(
      "ZZZZZZ  SEI  A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 "
      "P:nvMXdIZc Fr:0 Cycle:1 BC:78").has_value());

  REQUIRE_FALSE(ParseMesenCpuLine(
      "008000  SEI  A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 "
      "P:nvMXdIZ Fr:0 Cycle:1 BC:78").has_value());
}

TEST_CASE("ParsePupsnesHeader reads version and rom-sha1", "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::ParsePupsnesHeader;

  auto hdr = ParsePupsnesHeader(
      "# pupsnes-trace v1  rom-sha1=0123456789abcdef0123456789abcdef01234567  "
      "master-hz=21477272  lines-per-frame=262");
  REQUIRE(hdr.has_value());
  REQUIRE(hdr->version == 1U);
  REQUIRE(hdr->rom_sha1_hex == "0123456789abcdef0123456789abcdef01234567");
}

TEST_CASE("ParsePupsnesHeader rejects malformed or wrong-version headers",
          "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::ParsePupsnesHeader;

  REQUIRE_FALSE(ParsePupsnesHeader("# pupsnes-trace v2  rom-sha1=abc").has_value());
  REQUIRE_FALSE(ParsePupsnesHeader("008000  SEI ...").has_value());
  REQUIRE_FALSE(ParsePupsnesHeader("").has_value());
}

TEST_CASE("ParsePupsnesDataLine extracts all fields", "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::ParsePupsnesDataLine;

  constexpr const char* kLine =
      "00:8000  78                SEI                    "
      "A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc E:0  "
      "MT:000000000000  V:000 H:0048  #1";

  auto result = ParsePupsnesDataLine(kLine);
  REQUIRE(result.has_value());
  const auto& s = *result;
  REQUIRE(s.pb == 0x00);
  REQUIRE(s.pc == 0x8000);
  REQUIRE(s.A == 0x9400);
  REQUIRE(s.X == 0x0010);
  REQUIRE(s.Y == 0x0004);
  REQUIRE(s.S == 0x01FF);
  REQUIRE(s.D == 0x0000);
  REQUIRE(s.DB == 0x00);
  REQUIRE(s.a_hi_valid);
  REQUIRE(s.x_hi_valid);
  REQUIRE(s.y_hi_valid);
}

TEST_CASE("ParsePupsnesDataLine rejects non-data lines", "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::ParsePupsnesDataLine;

  REQUIRE_FALSE(ParsePupsnesDataLine("# pupsnes-trace v1  rom-sha1=abc").has_value());
  REQUIRE_FALSE(ParsePupsnesDataLine("").has_value());
  REQUIRE_FALSE(ParsePupsnesDataLine("00:8000  78  SEI  A:9400").has_value());
}

TEST_CASE("CompareStates detects per-field differences",
          "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::CompareStates;
  using pupsnes::debugger::trace_diff::CpuState;
  namespace fb = pupsnes::debugger::trace_diff::FieldBit;

  CpuState pup{};
  pup.pb = 0x00; pup.pc = 0x8000;
  pup.A = 0x9400; pup.X = 0x0010; pup.Y = 0x0004;
  pup.S = 0x01FF; pup.D = 0x0000; pup.DB = 0x00;
  pup.a_hi_valid = pup.x_hi_valid = pup.y_hi_valid = true;

  CpuState mes = pup;
  REQUIRE(CompareStates(pup, mes) == 0U);

  mes.X = 0x0011;
  REQUIRE((CompareStates(pup, mes) & fb::kX) != 0U);

  mes = pup;
  mes.flags.m = true;
  REQUIRE((CompareStates(pup, mes) & fb::kFlagM) != 0U);

  mes = pup;
  mes.A = 0x0000;
  mes.S = 0x0100;
  const uint32_t diff = CompareStates(pup, mes);
  REQUIRE((diff & fb::kA) != 0U);
  REQUIRE((diff & fb::kS) != 0U);
  REQUIRE((diff & fb::kX) == 0U);
}

TEST_CASE("CompareStates checks only low byte when Mesen hi-byte is invalid",
          "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::CompareStates;
  using pupsnes::debugger::trace_diff::CpuState;
  namespace fb = pupsnes::debugger::trace_diff::FieldBit;

  CpuState pup{};
  pup.A = 0xFF12; pup.a_hi_valid = true;
  pup.x_hi_valid = pup.y_hi_valid = true;

  CpuState mes = pup;
  mes.A = 0x0012;
  mes.a_hi_valid = false;
  REQUIRE((CompareStates(pup, mes) & fb::kA) == 0U);

  mes.A = 0x0013;
  REQUIRE((CompareStates(pup, mes) & fb::kA) != 0U);
}

namespace {

constexpr const char* kPupsnesTwoLines =
    "# pupsnes-trace v1  rom-sha1=aa  master-hz=21477272  lines-per-frame=262\n"
    "00:8000  78                SEI                    "
    "A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc E:0  "
    "MT:000000000000  V:000 H:0048  #1\n"
    "00:8001  9C 00 42          STZ $4200 [NMITIMEN]   "
    "A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc E:0  "
    "MT:000000000004  V:000 H:0052  #2\n";

constexpr const char* kMesenTwoLines =
    "008000  SEI                            "
    "A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc "
    "V:0   H:48  Fr:237 Cycle:1 BC:78\n"
    "FFC5  MOV (X),A [$00EF] = $00          "
    "A:00 X:EF Y:00 S:EF P:nvpbhiZc V:0   H:48\n"
    "008001  STZ $4200 [NMITIMEN] = $9C     "
    "A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc "
    "V:0   H:52  Fr:237 Cycle:3 BC:9C 00 42\n";

}  // namespace

TEST_CASE("Diff matches two aligned streams and skips APU lines",
          "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::Diff;
  using pupsnes::debugger::trace_diff::DiffOptions;
  using pupsnes::debugger::trace_diff::Exhausted;

  std::stringstream pup(kPupsnesTwoLines);
  std::stringstream mes(kMesenTwoLines);
  auto result = Diff(pup, mes, DiffOptions{});
  REQUIRE(result.pairs_matched == 2U);
  REQUIRE(result.mismatches.empty());
  REQUIRE(result.mesen_cpu_lines_seen == 2U);
  REQUIRE(result.mesen_skipped_lines == 1U);
  REQUIRE((result.exhausted == Exhausted::kBoth ||
           result.exhausted == Exhausted::kPupsnes ||
           result.exhausted == Exhausted::kMesen));
}

TEST_CASE("Diff reports a single-field mismatch", "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::Diff;
  using pupsnes::debugger::trace_diff::DiffOptions;
  namespace fb = pupsnes::debugger::trace_diff::FieldBit;

  std::string pup_text = kPupsnesTwoLines;
  pup_text.replace(pup_text.find("X:0010 Y:0004 S:01FF D:0000 DB:00 "
                                 "P:nvMXdIZc E:0  MT:000000000004"),
                   std::string("X:0010").size(), "X:0011");
  std::stringstream pup(pup_text);
  std::stringstream mes(kMesenTwoLines);
  auto result = Diff(pup, mes, DiffOptions{});
  REQUIRE(result.pairs_matched == 1U);
  REQUIRE(result.mismatches.size() == 1U);
  REQUIRE(result.mismatches[0].pair_index == 2U);
  REQUIRE((result.mismatches[0].fields & fb::kX) != 0U);
}

TEST_CASE("Diff succeeds when either stream ends early",
          "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::Diff;
  using pupsnes::debugger::trace_diff::DiffOptions;
  using pupsnes::debugger::trace_diff::Exhausted;

  constexpr const char* kMesenOne =
      "008000  SEI                            "
      "A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc "
      "V:0   H:48  Fr:237 Cycle:1 BC:78\n";
  {
    std::stringstream pup(kPupsnesTwoLines);
    std::stringstream mes(kMesenOne);
    auto result = Diff(pup, mes, DiffOptions{});
    REQUIRE(result.mismatches.empty());
    REQUIRE(result.pairs_matched == 1U);
    REQUIRE(result.exhausted == Exhausted::kMesen);
  }

  constexpr const char* kPupsnesOne =
      "# pupsnes-trace v1  rom-sha1=aa  master-hz=21477272  lines-per-frame=262\n"
      "00:8000  78                SEI                    "
      "A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc E:0  "
      "MT:000000000000  V:000 H:0048  #1\n";
  {
    std::stringstream pup(kPupsnesOne);
    std::stringstream mes(kMesenTwoLines);
    auto result = Diff(pup, mes, DiffOptions{});
    REQUIRE(result.mismatches.empty());
    REQUIRE(result.pairs_matched == 1U);
    REQUIRE(result.exhausted == Exhausted::kPupsnes);
  }
}

TEST_CASE("Diff respects max_mismatches", "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::Diff;
  using pupsnes::debugger::trace_diff::DiffOptions;

  std::string pup_text =
      "# pupsnes-trace v1  rom-sha1=aa  master-hz=21477272  lines-per-frame=262\n"
      "00:8000  78                SEI                    "
      "A:9400 X:0011 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc E:0  "
      "MT:000000000000  V:000 H:0048  #1\n"
      "00:8001  9C 00 42          STZ $4200              "
      "A:9400 X:0011 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc E:0  "
      "MT:000000000004  V:000 H:0052  #2\n";
  std::stringstream pup(pup_text);
  std::stringstream mes(kMesenTwoLines);
  DiffOptions opts;
  opts.max_mismatches = 5;
  auto result = Diff(pup, mes, opts);
  REQUIRE(result.mismatches.size() == 2U);
}

TEST_CASE("Diff captures last-3 pupsnes context before a mismatch",
          "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::Diff;
  using pupsnes::debugger::trace_diff::DiffOptions;

  constexpr const char* kPup =
      "# pupsnes-trace v1  rom-sha1=aa  master-hz=21477272  lines-per-frame=262\n"
      "00:8000  78  SEI   A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc E:0  MT:000000000000  V:000 H:0048  #1\n"
      "00:8001  9C  STZ   A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc E:0  MT:000000000004  V:000 H:0052  #2\n"
      "00:8004  9C  STZ   A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc E:0  MT:000000000008  V:000 H:0059  #3\n"
      "00:8007  9C  STZ   A:9400 X:0011 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc E:0  MT:00000000000C  V:000 H:0067  #4\n";
  constexpr const char* kMes =
      "008000  SEI   A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc V:0 H:48 Fr:237 Cycle:1 BC:78\n"
      "008001  STZ $4200   A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc V:0 H:52 Fr:237 Cycle:3 BC:9C\n"
      "008004  STZ $420C   A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc V:0 H:59 Fr:237 Cycle:7 BC:9C\n"
      "008007  STZ $420B   A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc V:0 H:67 Fr:237 Cycle:11 BC:9C\n";

  std::stringstream pup(kPup);
  std::stringstream mes(kMes);
  auto result = Diff(pup, mes, DiffOptions{});
  REQUIRE(result.mismatches.size() == 1U);
  const auto& m = result.mismatches[0];
  REQUIRE(m.pair_index == 4U);
  REQUIRE(m.context_pupsnes.size() == 3U);
  REQUIRE(m.context_pupsnes[0].find("#1") != std::string::npos);
  REQUIRE(m.context_pupsnes[1].find("#2") != std::string::npos);
  REQUIRE(m.context_pupsnes[2].find("#3") != std::string::npos);
}

TEST_CASE("Diff --align-on-first-pc skips to first common PC",
          "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::Diff;
  using pupsnes::debugger::trace_diff::DiffOptions;

  constexpr const char* kPup =
      "# pupsnes-trace v1  rom-sha1=aa  master-hz=21477272  lines-per-frame=262\n"
      "00:8000  78  SEI   A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc E:0  MT:000000000000  V:000 H:0048  #1\n"
      "00:8001  9C  STZ   A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc E:0  MT:000000000004  V:000 H:0052  #2\n";
  constexpr const char* kMes =
      "00FFC0  LDX #$EF  A:0000 X:00EF Y:0000 S:01FF D:0000 DB:00 P:nvMXdIZc V:0 H:0 Fr:0 Cycle:0 BC:A2\n"
      "00FFE0  JMP $8000 A:0000 X:00EF Y:0000 S:01FF D:0000 DB:00 P:nvMXdIZc V:0 H:8 Fr:0 Cycle:0 BC:4C\n"
      "008000  SEI       A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc V:0 H:48 Fr:237 Cycle:1 BC:78\n"
      "008001  STZ $4200 A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc V:0 H:52 Fr:237 Cycle:3 BC:9C\n";

  std::stringstream pup(kPup);
  std::stringstream mes(kMes);
  DiffOptions opts;
  opts.align_on_first_pc = true;
  auto result = Diff(pup, mes, opts);
  REQUIRE(result.parse_error.empty());
  REQUIRE(result.mismatches.empty());
  REQUIRE(result.pairs_matched == 2U);
}

TEST_CASE("FormatReport prints banner and OK summary",
          "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::Diff;
  using pupsnes::debugger::trace_diff::DiffOptions;
  using pupsnes::debugger::trace_diff::FormatReport;

  std::stringstream pup(kPupsnesTwoLines);
  std::stringstream mes(kMesenTwoLines);
  auto result = Diff(pup, mes, DiffOptions{});
  std::stringstream out;
  FormatReport(result, out);
  const std::string text = out.str();
  REQUIRE(text.find("pupsnes-trace-diff: comparing") != std::string::npos);
  REQUIRE(text.find("OK:") != std::string::npos);
  REQUIRE(text.find("pairs matched") != std::string::npos);
}

TEST_CASE("FormatReport prints a mismatch with differing fields",
          "[unit][trace_diff]") {
  using pupsnes::debugger::trace_diff::Diff;
  using pupsnes::debugger::trace_diff::DiffOptions;
  using pupsnes::debugger::trace_diff::FormatReport;

  std::string pup_text =
      "# pupsnes-trace v1  rom-sha1=aa  master-hz=21477272  lines-per-frame=262\n"
      "00:8000  78  SEI   A:9400 X:0010 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc E:0  MT:0 V:000 H:0048  #1\n"
      "00:8001  9C  STZ   A:9400 X:0011 Y:0004 S:01FF D:0000 DB:00 P:nvMXdIZc E:0  MT:0 V:000 H:0052  #2\n";
  std::stringstream pup(pup_text);
  std::stringstream mes(kMesenTwoLines);
  auto result = Diff(pup, mes, DiffOptions{});
  std::stringstream out;
  FormatReport(result, out);
  const std::string text = out.str();
  REQUIRE(text.find("MISMATCH") != std::string::npos);
  REQUIRE(text.find("X differs") != std::string::npos);
  REQUIRE(text.find("pupsnes=0011") != std::string::npos);
  REQUIRE(text.find("mesen=0010") != std::string::npos);
}
