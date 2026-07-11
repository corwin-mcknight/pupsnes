#pragma once

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>

#include "pupsnes/core/emu_event.h"
#include "pupsnes/debugger/sha1.h"

namespace pupsnes::debugger {

// Writes one versioned line per emulation event to a file. Format:
//   # pupsnes-events v1  rom-sha1=...  master-hz=...  lines-per-frame=...
//   MT:<12-hex>  V:<3> H:<4>  <CATEGORY>  <NAME>  <message>
// Events arrive in deterministic emission order (single-threaded machine),
// so output is byte-stable run-to-run for the same ROM and inputs. Errors
// latch like FileTraceSink: the first failure is kept and later events drop.
class FileEventSink : public EmuEventSink {
 public:
  static constexpr uint32_t kFormatVersion = 1;
  static constexpr uint64_t kMasterClockHz = 21477272U;
  static constexpr uint32_t kNominalLinesPerFrame = 262U;

  FileEventSink(const std::string& path, const Sha1Digest& rom_sha1);
  ~FileEventSink() override;

  void OnEmuEvent(const EmuEvent& event) override;
  void Flush();

  void SetCategoryMask(uint32_t mask) { category_mask_ = mask; }
  [[nodiscard]] uint32_t GetCategoryMask() const { return category_mask_; }
  [[nodiscard]] uint64_t LineCount() const { return line_count_; }
  [[nodiscard]] bool HasError() const { return error_.has_value(); }
  [[nodiscard]] const std::string& Error() const;

 private:
  void WriteHeader(const Sha1Digest& rom_sha1);
  void LatchError(std::string message);

  std::ofstream out_;
  uint64_t line_count_ = 0;
  uint32_t category_mask_ = kAllEmuEventCategoriesMask;
  std::optional<std::string> error_;
  std::string empty_error_;
};

}  // namespace pupsnes::debugger
