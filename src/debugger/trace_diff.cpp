#include "pupsnes/debugger/trace_diff.h"

#include <cctype>
#include <format>
#include <istream>
#include <ostream>
#include <utility>

namespace pupsnes::debugger::trace_diff {

namespace {

bool IsHexDigit(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }

bool MatchFlagLetter(char observed, char expected_lower, bool& out_set) {
  const char expected_upper = static_cast<char>(expected_lower - 32);
  if (observed == expected_lower) {
    out_set = false;
    return true;
  }
  if (observed == expected_upper) {
    out_set = true;
    return true;
  }
  return false;
}

std::optional<std::pair<uint32_t, size_t>> ParseHexRun(std::string_view s, size_t max_digits) {
  uint32_t value = 0;
  size_t i = 0;
  while (i < s.size() && i < max_digits && IsHexDigit(s[i])) {
    const char c = s[i];
    uint32_t d;
    if (c >= '0' && c <= '9') {
      d = static_cast<uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = static_cast<uint32_t>(c - 'a' + 10);
    } else {
      d = static_cast<uint32_t>(c - 'A' + 10);
    }
    value = (value << 4) | d;
    ++i;
  }
  if (i == 0) return std::nullopt;
  return std::make_pair(value, i);
}

size_t FindNamedField(std::string_view line, std::string_view key, size_t search_from) {
  const auto pos = line.find(key, search_from);
  if (pos == std::string_view::npos) return std::string_view::npos;
  return pos + key.size();
}

bool ReadNamedHex(std::string_view line, std::string_view key, size_t max_digits, uint32_t& out_value,
                  size_t* out_digits_seen = nullptr) {
  const size_t start = FindNamedField(line, key, 0);
  if (start == std::string_view::npos) return false;
  auto parsed = ParseHexRun(line.substr(start), max_digits);
  if (!parsed) return false;
  out_value = parsed->first;
  if (out_digits_seen != nullptr) *out_digits_seen = parsed->second;
  return true;
}

bool AxyEqual(uint16_t a, uint16_t b, bool a_hi_valid, bool b_hi_valid) {
  const uint16_t mask = (a_hi_valid && b_hi_valid) ? static_cast<uint16_t>(0xFFFFU) : static_cast<uint16_t>(0x00FFU);
  return static_cast<uint16_t>(a & mask) == static_cast<uint16_t>(b & mask);
}

}  // namespace

std::optional<CpuFlags> ParsePFlags(std::string_view s) {
  if (s.size() != 8) return std::nullopt;
  CpuFlags f{};
  constexpr char kLetters[8] = {'n', 'v', 'm', 'x', 'd', 'i', 'z', 'c'};
  bool* const targets[8] = {&f.n, &f.v, &f.m, &f.x, &f.d, &f.i, &f.z, &f.c};
  for (size_t i = 0; i < 8; ++i) {
    if (!MatchFlagLetter(s[i], kLetters[i], *targets[i])) return std::nullopt;
  }
  return f;
}

MesenLineKind ClassifyMesenLine(std::string_view line) {
  if (line.size() < 7) return MesenLineKind::kSkipped;
  for (size_t i = 0; i < 6; ++i) {
    if (!IsHexDigit(line[i])) return MesenLineKind::kSkipped;
  }
  if (line[6] != ' ' && line[6] != '\t') return MesenLineKind::kSkipped;
  if (line.find(" Fr:") == std::string_view::npos) return MesenLineKind::kSkipped;
  return MesenLineKind::kCpu;
}

std::optional<CpuState> ParseMesenCpuLine(std::string_view line) {
  auto pc24 = ParseHexRun(line, 6);
  if (!pc24 || pc24->second != 6) return std::nullopt;

  CpuState s{};
  s.pb = static_cast<uint8_t>((pc24->first >> 16) & 0xFFU);
  s.pc = static_cast<uint16_t>(pc24->first & 0xFFFFU);

  auto read_axy = [&](std::string_view key, uint16_t& out_reg, bool& hi_valid) {
    uint32_t value = 0;
    size_t digits = 0;
    if (!ReadNamedHex(line, key, 4, value, &digits)) return false;
    out_reg = static_cast<uint16_t>(value);
    hi_valid = (digits >= 3);
    return true;
  };
  if (!read_axy(" A:", s.A, s.a_hi_valid)) return std::nullopt;
  if (!read_axy(" X:", s.X, s.x_hi_valid)) return std::nullopt;
  if (!read_axy(" Y:", s.Y, s.y_hi_valid)) return std::nullopt;

  uint32_t s_val = 0;
  uint32_t d_val = 0;
  if (!ReadNamedHex(line, " S:", 4, s_val)) return std::nullopt;
  if (!ReadNamedHex(line, " D:", 4, d_val)) return std::nullopt;
  s.S = static_cast<uint16_t>(s_val);
  s.D = static_cast<uint16_t>(d_val);

  uint32_t db_val = 0;
  if (!ReadNamedHex(line, " DB:", 2, db_val)) return std::nullopt;
  s.DB = static_cast<uint8_t>(db_val);

  const size_t p_start = FindNamedField(line, " P:", 0);
  if (p_start == std::string_view::npos) return std::nullopt;
  if (p_start + 8 > line.size()) return std::nullopt;
  auto flags = ParsePFlags(line.substr(p_start, 8));
  if (!flags) return std::nullopt;
  s.flags = *flags;
  return s;
}

std::optional<PupsnesHeader> ParsePupsnesHeader(std::string_view line) {
  constexpr std::string_view kPrefix = "# pupsnes-trace v";
  if (line.size() < kPrefix.size()) return std::nullopt;
  if (line.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;

  PupsnesHeader h{};
  size_t i = kPrefix.size();
  uint32_t version = 0;
  while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
    version = version * 10 + static_cast<uint32_t>(line[i] - '0');
    ++i;
  }
  if (version != kSupportedPupsnesVersion) return std::nullopt;
  h.version = version;

  constexpr std::string_view kSha1Key = "rom-sha1=";
  const auto sha_pos = line.find(kSha1Key, i);
  if (sha_pos == std::string_view::npos) return std::nullopt;
  const size_t sha_start = sha_pos + kSha1Key.size();
  size_t sha_end = sha_start;
  while (sha_end < line.size() && IsHexDigit(line[sha_end])) ++sha_end;
  if (sha_end == sha_start) return std::nullopt;
  h.rom_sha1_hex.assign(line.data() + sha_start, sha_end - sha_start);
  return h;
}

std::optional<CpuState> ParsePupsnesDataLine(std::string_view line) {
  if (line.empty() || line.front() == '#') return std::nullopt;

  if (line.size() < 7) return std::nullopt;
  auto pb_val = ParseHexRun(line.substr(0, 2), 2);
  if (!pb_val || pb_val->second != 2) return std::nullopt;
  if (line[2] != ':') return std::nullopt;
  auto pc_val = ParseHexRun(line.substr(3, 4), 4);
  if (!pc_val || pc_val->second != 4) return std::nullopt;

  CpuState s{};
  s.pb = static_cast<uint8_t>(pb_val->first);
  s.pc = static_cast<uint16_t>(pc_val->first);

  uint32_t val = 0;
  size_t digits = 0;
  if (!ReadNamedHex(line, " A:", 4, val, &digits) || digits != 4) return std::nullopt;
  s.A = static_cast<uint16_t>(val);
  s.a_hi_valid = true;
  if (!ReadNamedHex(line, " X:", 4, val, &digits) || digits != 4) return std::nullopt;
  s.X = static_cast<uint16_t>(val);
  s.x_hi_valid = true;
  if (!ReadNamedHex(line, " Y:", 4, val, &digits) || digits != 4) return std::nullopt;
  s.Y = static_cast<uint16_t>(val);
  s.y_hi_valid = true;
  if (!ReadNamedHex(line, " S:", 4, val, &digits) || digits != 4) return std::nullopt;
  s.S = static_cast<uint16_t>(val);
  if (!ReadNamedHex(line, " D:", 4, val, &digits) || digits != 4) return std::nullopt;
  s.D = static_cast<uint16_t>(val);
  if (!ReadNamedHex(line, " DB:", 2, val, &digits) || digits != 2) return std::nullopt;
  s.DB = static_cast<uint8_t>(val);

  const size_t p_start = FindNamedField(line, " P:", 0);
  if (p_start == std::string_view::npos) return std::nullopt;
  if (p_start + 8 > line.size()) return std::nullopt;
  auto flags = ParsePFlags(line.substr(p_start, 8));
  if (!flags) return std::nullopt;
  s.flags = *flags;
  return s;
}

uint32_t CompareStates(const CpuState& pup, const CpuState& mes) {
  uint32_t d = 0;
  if (pup.pb != mes.pb) d |= FieldBit::kPB;
  if (pup.pc != mes.pc) d |= FieldBit::kPC;
  if (!AxyEqual(pup.A, mes.A, pup.a_hi_valid, mes.a_hi_valid)) d |= FieldBit::kA;
  if (!AxyEqual(pup.X, mes.X, pup.x_hi_valid, mes.x_hi_valid)) d |= FieldBit::kX;
  if (!AxyEqual(pup.Y, mes.Y, pup.y_hi_valid, mes.y_hi_valid)) d |= FieldBit::kY;
  if (pup.S != mes.S) d |= FieldBit::kS;
  if (pup.D != mes.D) d |= FieldBit::kD;
  if (pup.DB != mes.DB) d |= FieldBit::kDB;
  if (pup.flags.n != mes.flags.n) d |= FieldBit::kFlagN;
  if (pup.flags.v != mes.flags.v) d |= FieldBit::kFlagV;
  if (pup.flags.m != mes.flags.m) d |= FieldBit::kFlagM;
  if (pup.flags.x != mes.flags.x) d |= FieldBit::kFlagX;
  if (pup.flags.d != mes.flags.d) d |= FieldBit::kFlagD;
  if (pup.flags.i != mes.flags.i) d |= FieldBit::kFlagI;
  if (pup.flags.z != mes.flags.z) d |= FieldBit::kFlagZ;
  if (pup.flags.c != mes.flags.c) d |= FieldBit::kFlagC;
  return d;
}

namespace {

bool NextPupsnesState(std::istream& pup, size_t& line_number, std::string& raw, CpuState& out_state,
                      std::optional<PupsnesHeader>& out_header, std::string& out_error) {
  std::string line;
  while (std::getline(pup, line)) {
    ++line_number;
    if (line.empty()) continue;
    if (line.front() == '#') {
      if (!out_header) {
        auto hdr = ParsePupsnesHeader(line);
        if (hdr) out_header = std::move(hdr);
      }
      continue;
    }
    auto parsed = ParsePupsnesDataLine(line);
    if (!parsed) {
      out_error = "pupsnes line " + std::to_string(line_number) + ": malformed data line";
      return false;
    }
    raw = line;
    out_state = *parsed;
    return true;
  }
  return false;
}

bool NextMesenState(std::istream& mes, size_t& line_number, size_t& out_skipped, std::string& raw, CpuState& out_state,
                    std::string& out_error) {
  std::string line;
  while (std::getline(mes, line)) {
    ++line_number;
    switch (ClassifyMesenLine(line)) {
      case MesenLineKind::kSkipped: ++out_skipped; continue;
      case MesenLineKind::kCpu: {
        auto parsed = ParseMesenCpuLine(line);
        if (!parsed) {
          out_error = "mesen line " + std::to_string(line_number) + ": malformed CPU line";
          return false;
        }
        raw = line;
        out_state = *parsed;
        return true;
      }
    }
  }
  return false;
}

constexpr size_t kContextDepth = 3;

void PushContext(std::vector<std::string>& recent, const std::string& raw) {
  if (recent.size() == kContextDepth) {
    recent.erase(recent.begin());
  }
  recent.push_back(raw);
}

}  // namespace

DiffResult Diff(std::istream& pupsnes, std::istream& mesen, const DiffOptions& options) {
  DiffResult r{};
  size_t pup_line = 0;
  size_t mes_line = 0;

  auto pup_next = [&](std::string& raw, CpuState& s) {
    bool ok = NextPupsnesState(pupsnes, pup_line, raw, s, r.pupsnes_header, r.parse_error);
    if (ok) ++r.pupsnes_data_lines_seen;
    return ok;
  };
  auto mes_next = [&](std::string& raw, CpuState& s) {
    bool ok = NextMesenState(mesen, mes_line, r.mesen_skipped_lines, raw, s, r.parse_error);
    if (ok) ++r.mesen_cpu_lines_seen;
    return ok;
  };

  std::string junk_raw;
  CpuState junk{};
  for (size_t i = 0; i < options.skip_pupsnes; ++i) {
    if (!pup_next(junk_raw, junk)) {
      r.exhausted = Exhausted::kPupsnes;
      return r;
    }
    if (!r.parse_error.empty()) return r;
  }
  for (size_t i = 0; i < options.skip_mesen; ++i) {
    if (!mes_next(junk_raw, junk)) {
      r.exhausted = Exhausted::kMesen;
      return r;
    }
    if (!r.parse_error.empty()) return r;
  }

  std::vector<std::string> recent_pup;

  // Optional alignment phase: pin pupsnes's first data line as the anchor PC
  // and advance Mesen until its PC matches. The common case is Mesen carrying
  // extra reset / power-on lines that pupsnes' own boot doesn't emit; users
  // with the opposite skew should use --skip-pupsnes instead.
  if (options.align_on_first_pc) {
    std::string pup_buf_raw;
    std::string mes_buf_raw;
    CpuState pup_buf{};
    CpuState mes_buf{};
    bool have_pup = pup_next(pup_buf_raw, pup_buf);
    if (!r.parse_error.empty()) return r;
    if (!have_pup) {
      r.exhausted = Exhausted::kPupsnes;
      return r;
    }
    bool have_mes = mes_next(mes_buf_raw, mes_buf);
    if (!r.parse_error.empty()) return r;
    while (have_mes) {
      if (mes_buf.pb == pup_buf.pb && mes_buf.pc == pup_buf.pc) break;
      have_mes = mes_next(mes_buf_raw, mes_buf);
      if (!r.parse_error.empty()) return r;
    }
    if (!have_mes) {
      r.exhausted = Exhausted::kMesen;
      return r;
    }
    const uint32_t diff = CompareStates(pup_buf, mes_buf);
    if (diff == 0) {
      ++r.pairs_matched;
      PushContext(recent_pup, pup_buf_raw);
    } else {
      Mismatch m{};
      m.pair_index = 1;
      m.pupsnes_line_number = pup_line;
      m.mesen_line_number = mes_line;
      m.fields = diff;
      m.pupsnes_state = pup_buf;
      m.mesen_state = mes_buf;
      m.pupsnes_raw = pup_buf_raw;
      m.mesen_raw = mes_buf_raw;
      m.context_pupsnes = recent_pup;
      r.mismatches.push_back(std::move(m));
      if (r.mismatches.size() >= options.max_mismatches) return r;
    }
  }

  std::string pup_raw;
  std::string mes_raw;
  CpuState pup_state{};
  CpuState mes_state{};
  while (true) {
    const bool have_pup = pup_next(pup_raw, pup_state);
    if (!r.parse_error.empty()) return r;
    const bool have_mes = mes_next(mes_raw, mes_state);
    if (!r.parse_error.empty()) return r;
    if (!have_pup && !have_mes) {
      r.exhausted = Exhausted::kBoth;
      return r;
    }
    if (!have_pup) {
      r.exhausted = Exhausted::kPupsnes;
      return r;
    }
    if (!have_mes) {
      r.exhausted = Exhausted::kMesen;
      return r;
    }

    const uint32_t diff = CompareStates(pup_state, mes_state);
    if (diff == 0) {
      ++r.pairs_matched;
      PushContext(recent_pup, pup_raw);
    } else {
      Mismatch m{};
      m.pair_index = r.pairs_matched + r.mismatches.size() + 1;
      m.pupsnes_line_number = pup_line;
      m.mesen_line_number = mes_line;
      m.fields = diff;
      m.pupsnes_state = pup_state;
      m.mesen_state = mes_state;
      m.pupsnes_raw = pup_raw;
      m.mesen_raw = mes_raw;
      m.context_pupsnes = recent_pup;
      r.mismatches.push_back(std::move(m));
      if (r.mismatches.size() >= options.max_mismatches) {
        r.exhausted = Exhausted::kNeither;
        return r;
      }
    }
  }
}

namespace {

template <typename T>
void PrintRegDiff(std::ostream& out, const char* name, T pup_v, T mes_v) {
  // Width follows the type: uint8_t → 2 hex digits, uint16_t → 4.
  constexpr int kWidth = sizeof(T) * 2;
  out << "  " << name << " differs (pupsnes=" << std::format("{:0{}X}", static_cast<unsigned>(pup_v), kWidth)
      << ", mesen=" << std::format("{:0{}X}", static_cast<unsigned>(mes_v), kWidth) << ")\n";
}

void PrintFlagDiff(std::ostream& out, const char* name, bool pup_v, bool mes_v) {
  out << "  flag " << name << " differs (pupsnes=" << (pup_v ? "1" : "0") << ", mesen=" << (mes_v ? "1" : "0") << ")\n";
}

void PrintMismatch(std::ostream& out, const Mismatch& m) {
  out << "MISMATCH at pair " << m.pair_index << " (pupsnes line " << m.pupsnes_line_number << ", mesen line "
      << m.mesen_line_number << ")\n";
  out << "  pupsnes: " << m.pupsnes_raw << "\n";
  out << "  mesen:   " << m.mesen_raw << "\n";

  // Tables of (bit, name, getter) per width. A field gets printed iff its bit is set in m.fields.
  struct Reg16Field {
    uint32_t bit;
    const char* name;
    uint16_t CpuState::* getter;
  };
  static constexpr Reg16Field kReg16s[] = {
      {FieldBit::kPC, "PC", &CpuState::pc}, {FieldBit::kA, "A", &CpuState::A}, {FieldBit::kX, "X", &CpuState::X},
      {FieldBit::kY, "Y", &CpuState::Y},    {FieldBit::kS, "S", &CpuState::S}, {FieldBit::kD, "D", &CpuState::D},
  };

  struct FlagField {
    uint32_t bit;
    const char* name;
    bool CpuFlags::* getter;
  };
  static constexpr FlagField kFlags[] = {
      {FieldBit::kFlagN, "N", &CpuFlags::n}, {FieldBit::kFlagV, "V", &CpuFlags::v},
      {FieldBit::kFlagM, "M", &CpuFlags::m}, {FieldBit::kFlagX, "X", &CpuFlags::x},
      {FieldBit::kFlagD, "D", &CpuFlags::d}, {FieldBit::kFlagI, "I", &CpuFlags::i},
      {FieldBit::kFlagZ, "Z", &CpuFlags::z}, {FieldBit::kFlagC, "C", &CpuFlags::c},
  };

  // Print PB before PC, DB after kY etc., to preserve the legacy field ordering.
  if ((m.fields & FieldBit::kPB) != 0U) PrintRegDiff(out, "PB", m.pupsnes_state.pb, m.mesen_state.pb);
  for (const auto& f : kReg16s) {
    if ((m.fields & f.bit) != 0U) PrintRegDiff(out, f.name, m.pupsnes_state.*f.getter, m.mesen_state.*f.getter);
  }
  if ((m.fields & FieldBit::kDB) != 0U) PrintRegDiff(out, "DB", m.pupsnes_state.DB, m.mesen_state.DB);
  for (const auto& f : kFlags) {
    if ((m.fields & f.bit) != 0U) {
      PrintFlagDiff(out, f.name, m.pupsnes_state.flags.*f.getter, m.mesen_state.flags.*f.getter);
    }
  }

  if (!m.context_pupsnes.empty()) {
    out << "Context (last " << m.context_pupsnes.size() << " matching pupsnes lines):\n";
    for (const auto& line : m.context_pupsnes) {
      out << "  " << line << "\n";
    }
  }
}

const char* ExhaustedName(Exhausted e) {
  switch (e) {
    case Exhausted::kNeither: return "neither";
    case Exhausted::kPupsnes: return "pupsnes";
    case Exhausted::kMesen: return "mesen";
    case Exhausted::kBoth: return "both";
  }
  return "?";
}

}  // namespace

void FormatReport(const DiffResult& r, std::ostream& out) {
  out << "pupsnes-trace-diff: comparing\n";
  out << "  pupsnes data lines:    " << r.pupsnes_data_lines_seen << "\n";
  out << "  mesen cpu lines:       " << r.mesen_cpu_lines_seen << "\n";
  out << "  mesen skipped lines:   " << r.mesen_skipped_lines << "\n";
  if (r.pupsnes_header) {
    out << "  pupsnes rom-sha1:      " << r.pupsnes_header->rom_sha1_hex << "\n";
  }
  out << "\n";

  if (!r.parse_error.empty()) {
    out << "PARSE ERROR: " << r.parse_error << "\n";
    return;
  }

  if (r.mismatches.empty()) {
    out << "OK: " << r.pairs_matched << " pairs matched; first exhausted side: " << ExhaustedName(r.exhausted) << ".\n";
    return;
  }

  for (const auto& m : r.mismatches) {
    PrintMismatch(out, m);
    out << "\n";
  }
  out << "TOTAL: " << r.pairs_matched << " pairs matched before mismatch; " << r.mismatches.size()
      << " mismatch(es) reported.\n";
}

}  // namespace pupsnes::debugger::trace_diff
