#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>

#include "pupsnes/debugger/sha1.h"
#include "pupsnes/core/debugger_contract.h"

namespace pupsnes {
class SNES;
}

namespace pupsnes::debugger {

// Writes one versioned line per retired instruction to a file. Format:
//   # pupsnes-trace v1  rom-sha1=...  master-hz=...  lines-per-frame=...
//   PB:PPPP  OP OP OP OP  MNEMONIC OPERAND           A:... X:... ...  MT:...  V:... H:...  #1
// See docs/superpowers/specs/2026-04-23-debugger-trace-log-emit-design.md
// for the column spec. Writer is deterministic and side-effect-free — V/H
// are derived from master_time, not queried from the PPU.
class FileTraceSink : public TraceSink {
 public:
  static constexpr uint32_t kFormatVersion = 1;
  static constexpr uint64_t kMasterClockHz = 21477272U;
  static constexpr uint32_t kNominalLinesPerFrame = 262U;
  static constexpr uint32_t kNominalMcycPerLine = 1364U;

  FileTraceSink(const SNES& snes, const std::string& path, const Sha1Digest& rom_sha1);
  ~FileTraceSink() override;

  void Record(const TraceEntry& entry) override;
  void Flush();

  [[nodiscard]] uint64_t LineCount() const { return retired_seq_; }
  [[nodiscard]] bool HasError() const { return error_.has_value(); }
  [[nodiscard]] const std::string& Error() const;

 private:
  void WriteHeader(const Sha1Digest& rom_sha1);
  void FormatLine(const TraceEntry& entry, std::string& out) const;
  void LatchError(std::string message);

  const SNES& snes_;
  std::ofstream out_;
  std::string line_buffer_;
  uint64_t retired_seq_ = 0;
  std::optional<std::string> error_;
  std::string empty_error_;
};

}  // namespace pupsnes::debugger
