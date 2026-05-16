#include "pupsnes/debugger/file_trace_sink.h"

#include <array>
#include <cstdio>
#include <ios>
#include <utility>

#include "pupsnes/debugger/disasm.h"
#include "pupsnes/core/snes.h"

namespace pupsnes::debugger {

namespace {

constexpr std::size_t kMnemonicColumnWidth = 22;

std::string FormatOpcodeBytesColumn(const DisassembledInstruction& d) {
  std::string out;
  for (uint8_t i = 0; i < 4; ++i) {
    if (i > 0) out.push_back(' ');
    if (i < d.byte_count) {
      static constexpr char kHex[] = "0123456789ABCDEF";
      const uint8_t byte = d.bytes[i];
      out.push_back(kHex[(byte >> 4) & 0x0F]);
      out.push_back(kHex[byte & 0x0F]);
    } else {
      out.push_back(' ');
      out.push_back(' ');
    }
  }
  return out;  // exactly 11 chars: "XX XX XX XX" or "XX          "
}

void AppendFlagLetter(std::string& out, char letter, bool set) {
  // Upper when set, lower when clear. Matches bsnes-plus.
  const char upper = static_cast<char>(letter - 32);
  out.push_back(set ? upper : letter);
}

}  // namespace

FileTraceSink::FileTraceSink(const SNES& snes, const std::string& path, const Sha1Digest& rom_sha1)
    : snes_(snes),
      out_(path, std::ios::out | std::ios::binary | std::ios::trunc) {
  if (!out_.is_open() || !out_.good()) {
    LatchError("failed to open " + path);
    return;
  }
  WriteHeader(rom_sha1);
  line_buffer_.reserve(256);
}

FileTraceSink::~FileTraceSink() { Flush(); }

void FileTraceSink::Flush() {
  if (out_.is_open()) {
    out_.flush();
  }
}

const std::string& FileTraceSink::Error() const {
  return error_.has_value() ? *error_ : empty_error_;
}

void FileTraceSink::LatchError(std::string message) {
  if (error_.has_value()) return;
  error_ = std::move(message);
}

void FileTraceSink::WriteHeader(const Sha1Digest& rom_sha1) {
  out_ << "# pupsnes-trace v" << kFormatVersion << "  rom-sha1=";
  for (uint8_t byte : rom_sha1) {
    static constexpr char kHex[] = "0123456789abcdef";
    out_ << kHex[(byte >> 4) & 0x0F] << kHex[byte & 0x0F];
  }
  out_ << "  master-hz=" << kMasterClockHz
       << "  lines-per-frame=" << kNominalLinesPerFrame
       << '\n';
  if (!out_.good()) LatchError("header write failed");
}

void FileTraceSink::FormatLine(const TraceEntry& entry, std::string& out) const {
  out.clear();
  const DisassembledInstruction d = DisassembleInstructionRaw(snes_, entry.pc, entry.regs.P);

  char scratch[64];

  // PBR:PC (7)
  std::snprintf(scratch, sizeof(scratch), "%02X:%04X",
                static_cast<unsigned>((entry.pc >> 16) & 0xFFU),
                static_cast<unsigned>(entry.pc & 0xFFFFU));
  out += scratch;
  out += "  ";

  // Opcode bytes column (11)
  out += FormatOpcodeBytesColumn(d);
  out += "  ";

  // Mnemonic+operand, left-justified, padded to kMnemonicColumnWidth
  const std::string disasm_text = FormatDisassembly(d);
  out += disasm_text;
  if (disasm_text.size() < kMnemonicColumnWidth) {
    out.append(kMnemonicColumnWidth - disasm_text.size(), ' ');
  }
  out += "  ";

  // Registers A/X/Y/S/D/DB
  std::snprintf(scratch, sizeof(scratch), "A:%04X X:%04X Y:%04X S:%04X D:%04X DB:%02X",
                static_cast<unsigned>(entry.regs.A),
                static_cast<unsigned>(entry.regs.X),
                static_cast<unsigned>(entry.regs.Y),
                static_cast<unsigned>(entry.regs.SP),
                static_cast<unsigned>(entry.regs.DP),
                static_cast<unsigned>(entry.regs.DBR));
  out += scratch;
  out += ' ';

  // P flags
  out += "P:";
  AppendFlagLetter(out, 'n', entry.regs.P.N);
  AppendFlagLetter(out, 'v', entry.regs.P.V);
  AppendFlagLetter(out, 'm', entry.regs.P.M);
  AppendFlagLetter(out, 'x', entry.regs.P.X);
  AppendFlagLetter(out, 'd', entry.regs.P.D);
  AppendFlagLetter(out, 'i', entry.regs.P.I);
  AppendFlagLetter(out, 'z', entry.regs.P.Z);
  AppendFlagLetter(out, 'c', entry.regs.P.C);
  out += ' ';

  // E flag as its own column
  std::snprintf(scratch, sizeof(scratch), "E:%u", entry.regs.P.E ? 1U : 0U);
  out += scratch;
  out += "  ";

  // MT: master_time in 12-hex (48 bits)
  std::snprintf(scratch, sizeof(scratch), "MT:%012llX",
                static_cast<unsigned long long>(entry.master_time));
  out += scratch;
  out += "  ";

  // V / H derived from master_time
  const uint64_t total_lines = entry.master_time / kNominalMcycPerLine;
  const uint32_t v = static_cast<uint32_t>(total_lines % kNominalLinesPerFrame);
  const uint32_t h = static_cast<uint32_t>(entry.master_time % kNominalMcycPerLine);
  std::snprintf(scratch, sizeof(scratch), "V:%03u H:%04u", v, h);
  out += scratch;
  out += "  ";

  // #<retired_seq>
  std::snprintf(scratch, sizeof(scratch), "#%llu",
                static_cast<unsigned long long>(retired_seq_));
  out += scratch;
}

void FileTraceSink::Record(const TraceEntry& entry) {
  if (error_.has_value()) return;
  ++retired_seq_;
  FormatLine(entry, line_buffer_);
  out_ << line_buffer_ << '\n';
  if (!out_.good()) {
    LatchError("trace write failed");
  }
}

}  // namespace pupsnes::debugger
