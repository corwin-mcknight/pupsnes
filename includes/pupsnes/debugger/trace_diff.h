#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pupsnes::debugger::trace_diff {

inline constexpr uint32_t kLibraryVersion = 1;

struct CpuFlags {
  bool n = false;
  bool v = false;
  bool m = false;
  bool x = false;
  bool d = false;
  bool i = false;
  bool z = false;
  bool c = false;

  friend bool operator==(const CpuFlags&, const CpuFlags&) = default;
};

struct CpuState {
  uint8_t pb = 0;
  uint16_t pc = 0;
  uint16_t A = 0;
  uint16_t X = 0;
  uint16_t Y = 0;
  uint16_t S = 0;
  uint16_t D = 0;
  uint8_t DB = 0;
  CpuFlags flags{};

  // True when the parser observed a 16-bit-wide value for the register. When
  // false (Mesen 8-bit mode), the high byte of A/X/Y is unknown and MUST NOT
  // be compared. `operator==` ignores these flags — strict equality is only
  // correct when both sides have valid high bytes.
  bool a_hi_valid = false;
  bool x_hi_valid = false;
  bool y_hi_valid = false;

  friend bool operator==(const CpuState& lhs, const CpuState& rhs) {
    return lhs.pb == rhs.pb && lhs.pc == rhs.pc && lhs.A == rhs.A &&
           lhs.X == rhs.X && lhs.Y == rhs.Y && lhs.S == rhs.S &&
           lhs.D == rhs.D && lhs.DB == rhs.DB && lhs.flags == rhs.flags;
  }
};

// Parse an 8-character P-column substring like "nvMXdIZc" into a CpuFlags.
// Returns nullopt if the length is not 8 or any letter is not one of nvmxdizc
// in either case. Both pupsnes and Mesen use this exact letter scheme.
std::optional<CpuFlags> ParsePFlags(std::string_view eight_letters);

enum class MesenLineKind : uint8_t { kCpu, kSkipped };

// Returns kCpu when `line` begins with [0-9A-Fa-f]{6} followed by whitespace
// AND contains the substring " Fr:" somewhere after. Otherwise kSkipped.
// APU lines, blank lines, and any unknown shape all return kSkipped.
MesenLineKind ClassifyMesenLine(std::string_view line);

// Parse a Mesen 65C816 CPU trace line into a CpuState. Returns nullopt if the
// line is malformed (missing required token, non-hex digits, etc). Caller
// must have verified ClassifyMesenLine(line) == kCpu first.
std::optional<CpuState> ParseMesenCpuLine(std::string_view line);

struct PupsnesHeader {
  uint32_t version = 0;
  std::string rom_sha1_hex;
};

inline constexpr uint32_t kSupportedPupsnesVersion = 1;

// Parse a pupsnes-trace header line ("# pupsnes-trace v1 ..."). Returns
// nullopt if the line doesn't match the expected prefix or the version is
// not kSupportedPupsnesVersion.
std::optional<PupsnesHeader> ParsePupsnesHeader(std::string_view line);

// Parse a pupsnes-trace data line ("PB:PC  OP ...  #seq") into a CpuState.
// Comment/header lines (leading '#') return nullopt; so do malformed lines.
std::optional<CpuState> ParsePupsnesDataLine(std::string_view line);

// Bit flags enumerating which fields differ between two CpuStates. Compose
// with bitwise or. Returned by CompareStates.
namespace FieldBit {
inline constexpr uint32_t kPB = 1U << 0;
inline constexpr uint32_t kPC = 1U << 1;
inline constexpr uint32_t kA = 1U << 2;
inline constexpr uint32_t kX = 1U << 3;
inline constexpr uint32_t kY = 1U << 4;
inline constexpr uint32_t kS = 1U << 5;
inline constexpr uint32_t kD = 1U << 6;
inline constexpr uint32_t kDB = 1U << 7;
inline constexpr uint32_t kFlagN = 1U << 8;
inline constexpr uint32_t kFlagV = 1U << 9;
inline constexpr uint32_t kFlagM = 1U << 10;
inline constexpr uint32_t kFlagX = 1U << 11;
inline constexpr uint32_t kFlagD = 1U << 12;
inline constexpr uint32_t kFlagI = 1U << 13;
inline constexpr uint32_t kFlagZ = 1U << 14;
inline constexpr uint32_t kFlagC = 1U << 15;
}  // namespace FieldBit

// Compare pupsnes vs mesen states and return a bitmap of differing fields.
// For A/X/Y: if either side has hi_byte_valid=false, only the low byte is
// compared for that register.
uint32_t CompareStates(const CpuState& pup, const CpuState& mes);

struct DiffOptions {
  size_t max_mismatches = 1;
  size_t skip_pupsnes = 0;
  size_t skip_mesen = 0;
  bool align_on_first_pc = false;
};

enum class Exhausted : uint8_t { kNeither, kPupsnes, kMesen, kBoth };

struct Mismatch {
  size_t pair_index = 0;
  size_t pupsnes_line_number = 0;
  size_t mesen_line_number = 0;
  uint32_t fields = 0;
  CpuState pupsnes_state{};
  CpuState mesen_state{};
  std::string pupsnes_raw;
  std::string mesen_raw;
  std::vector<std::string> context_pupsnes;
};

struct DiffResult {
  size_t pairs_matched = 0;
  size_t pupsnes_data_lines_seen = 0;
  size_t mesen_cpu_lines_seen = 0;
  size_t mesen_skipped_lines = 0;
  std::optional<PupsnesHeader> pupsnes_header;
  Exhausted exhausted = Exhausted::kNeither;
  std::vector<Mismatch> mismatches;
  std::string parse_error;
};

// Streaming diff. Reads line-by-line from both streams. Never buffers the
// whole input in memory. Returns DiffResult with per-field mismatches (up to
// DiffOptions::max_mismatches). parse_error is set and the walker returns
// early if any input line fails to parse.
DiffResult Diff(std::istream& pupsnes, std::istream& mesen,
                const DiffOptions& options);

// Write a human-readable report of a DiffResult to `out`. Always includes the
// banner. On match prints "OK:" with counts; on mismatch prints the first N
// mismatches with field-level details and pupsnes context.
void FormatReport(const DiffResult& result, std::ostream& out);

}  // namespace pupsnes::debugger::trace_diff
