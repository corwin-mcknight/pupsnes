#include "pupsnes/debugger/file_event_sink.h"

#include <ios>
#include <string>
#include <utility>

#include "pupsnes/debugger/emu_event_format.h"

namespace pupsnes::debugger {

FileEventSink::FileEventSink(const std::string& path, const Sha1Digest& rom_sha1)
    : out_(path, std::ios::out | std::ios::binary | std::ios::trunc) {
  if (!out_.is_open() || !out_.good()) {
    LatchError("failed to open " + path);
    return;
  }
  WriteHeader(rom_sha1);
}

FileEventSink::~FileEventSink() { Flush(); }

void FileEventSink::Flush() {
  if (out_.is_open()) {
    out_.flush();
  }
}

const std::string& FileEventSink::Error() const { return error_.has_value() ? *error_ : empty_error_; }

void FileEventSink::LatchError(std::string message) {
  if (error_.has_value()) return;
  error_ = std::move(message);
}

void FileEventSink::WriteHeader(const Sha1Digest& rom_sha1) {
  out_ << "# pupsnes-events v" << kFormatVersion << "  rom-sha1=";
  for (uint8_t byte : rom_sha1) {
    static constexpr char kHex[] = "0123456789abcdef";
    out_ << kHex[(byte >> 4) & 0x0F] << kHex[byte & 0x0F];
  }
  out_ << "  master-hz=" << kMasterClockHz << "  lines-per-frame=" << kNominalLinesPerFrame << '\n';
  if (!out_.good()) LatchError("header write failed");
}

void FileEventSink::OnEmuEvent(const EmuEvent& event) {
  if (error_.has_value()) return;
  if ((category_mask_ & EmuEventCategoryBit(EmuEventCategoryOf(event.kind))) == 0U) {
    return;
  }
  out_ << FormatEmuEventLine(event) << '\n';
  ++line_count_;
  if (!out_.good()) {
    LatchError("event write failed");
  }
}

}  // namespace pupsnes::debugger
