#include "pupsnes/hw/apu/spc_file.h"

#include <algorithm>
#include <bit>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pupsnes {
namespace {
uint32_t ReadLe(std::span<const uint8_t> bytes) {
  uint32_t value = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i) value |= static_cast<uint32_t>(bytes[i]) << (i * 8U);
  return value;
}

std::optional<uint32_t> ReadDecimal(std::span<const uint8_t> bytes) {
  uint32_t value = 0;
  bool padding = false;
  for (const auto byte : bytes) {
    if (byte == 0 || byte == ' ') {
      padding = true;
    } else if (!padding && byte >= '0' && byte <= '9') {
      value = value * 10 + byte - '0';
    } else {
      return std::nullopt;
    }
  }
  return value;
}

std::string ReadText(std::span<const uint8_t> bytes) {
  std::string text;
  for (const auto byte : bytes) {
    if (byte == 0) break;
    // ID666 has no dependable encoding. Keep the basic display fields safe
    // for the frontend's UTF-8 text renderer, replacing non-ASCII bytes.
    text += byte >= 32 && byte < 127 ? static_cast<char>(byte) : '?';
  }
  while (!text.empty() && text.back() == ' ') text.pop_back();
  return text;
}

void ReadExtended(SpcFile& file, std::span<const uint8_t> bytes) {
  if (bytes.size() < 0x10208 || ReadLe(bytes.subspan(0x10200, 4)) != 0x36646978U) return;  // xid6
  const auto size = ReadLe(bytes.subspan(0x10204, 4));
  if (size > bytes.size() - 0x10208) throw std::invalid_argument("Incomplete SPC xid6 metadata");
  const auto data = bytes.subspan(0x10208, size);
  std::optional<uint32_t> intro;
  std::optional<uint32_t> loop;
  int32_t end = 0;
  uint32_t loops = 2;
  for (std::size_t pos = 0; pos < data.size();) {
    if (data.size() - pos < 4) throw std::invalid_argument("Incomplete SPC xid6 item");
    const auto id = data[pos];
    const auto type = data[pos + 1];
    const auto value = ReadLe(data.subspan(pos + 2, 2));
    const auto length = type == 0 ? 0U : value;
    pos += 4;
    if (length > data.size() - pos) throw std::invalid_argument("Incomplete SPC xid6 payload");
    const auto payload = data.subspan(pos, length);
    if (type == 1 && id == 1) file.title = ReadText(payload);
    if (type == 1 && id == 2) file.game = ReadText(payload);
    if (type == 1 && id == 0x10) file.album = ReadText(payload);
    if (type == 0 && id == 0x35) loops = std::min(value, 255U);
    if (type == 4 && length == 4) {
      const auto ticks = ReadLe(payload);
      // Timing values are ticks of 1/64000 second; each native frame is two ticks.
      if (id == 0x30 && ticks <= 383999999U) intro = ticks;
      if (id == 0x31 && ticks <= 383999999U) loop = ticks;
      if (id == 0x32) end = std::bit_cast<int32_t>(ticks);
      if (id == 0x33 && ticks <= 383999999U) file.fade_frames = (static_cast<uint64_t>(ticks) + 1) / 2;
    }
    pos += length;
    // Some historical writers omit padding; skip only actual zero padding.
    while (pos < data.size() && pos % 4 != 0 && data[pos] == 0) ++pos;
  }
  if (intro || loop) {
    const int64_t ticks =
        static_cast<int64_t>(intro.value_or(0)) + static_cast<int64_t>(loop.value_or(0)) * loops + end;
    if (ticks > 0) file.play_frames = (static_cast<uint64_t>(ticks) + 1) / 2;
  }
}
}  // namespace

std::optional<uint64_t> SpcFile::EndFrame() const {
  if (!play_frames) return std::nullopt;
  return *play_frames + fade_frames;
}

bool SpcFile::Finished(uint64_t frames) const {
  const auto end = EndFrame();
  return end && frames >= *end;
}

int16_t SpcFile::FadeSample(int16_t sample, uint64_t frame) const {
  if (!play_frames || frame < *play_frames) return sample;
  if (fade_frames == 0 || frame - *play_frames >= fade_frames) return 0;
  const auto remaining = fade_frames - (frame - *play_frames);
  return static_cast<int16_t>(static_cast<int64_t>(sample) * static_cast<int64_t>(remaining) /
                              static_cast<int64_t>(fade_frames));
}

SpcFile SpcFile::Parse(std::span<const uint8_t> bytes) {
  constexpr std::string_view kSignature = "SNES-SPC700 Sound File Data v0.30";
  if (bytes.size() < 0x10180) throw std::invalid_argument("Incomplete SPC snapshot");
  if (!std::equal(kSignature.begin(), kSignature.end(), bytes.begin()) || bytes[0x21] != 0x1A || bytes[0x22] != 0x1A ||
      (bytes[0x23] != 0x1A && bytes[0x23] != 0x1B)) {
    throw std::invalid_argument("Invalid SPC v0.30 header");
  }
  SpcFile file;
  file.cpu.pc = static_cast<uint16_t>(bytes[0x25] | (static_cast<unsigned>(bytes[0x26]) << 8U));
  file.cpu.a = bytes[0x27];
  file.cpu.x = bytes[0x28];
  file.cpu.y = bytes[0x29];
  file.cpu.psw = bytes[0x2A];
  file.cpu.sp = bytes[0x2B];
  std::copy_n(bytes.begin() + 0x100, file.ram.size(), file.ram.begin());
  std::copy_n(bytes.begin() + 0x10100, file.dsp.size(), file.dsp.begin());
  // Full dumps preserve the RAM hidden by the IPL ROM in the extra RAM area.
  // Short legacy dumps omit it; retain their main RAM image in that case.
  if ((file.ram[0xF1] & 0x80U) != 0 && bytes.size() >= 0x10200) {
    std::copy_n(bytes.begin() + 0x101C0, 64, file.ram.begin() + 0xFFC0);
  }
  if (bytes[0x23] == 0x1A) {
    file.title = ReadText(bytes.subspan(0x2E, 32));
    file.game = ReadText(bytes.subspan(0x4E, 32));
    // ID666 does not flag text vs binary. Prefer decimal fields when both
    // timing fields contain only digits and padding; otherwise use binary.
    const auto text_seconds = ReadDecimal(bytes.subspan(0xA9, 3));
    const auto text_fade = ReadDecimal(bytes.subspan(0xAC, 5));
    const bool text = text_seconds && text_fade;
    const uint64_t seconds = text ? *text_seconds : ReadLe(bytes.subspan(0xA9, 3));
    const uint64_t fade_ms = text ? *text_fade : ReadLe(bytes.subspan(0xAC, 4));
    if (seconds != 0) file.play_frames = seconds * 32000;
    file.fade_frames = fade_ms * 32;
  }
  ReadExtended(file, bytes);
  return file;
}

}  // namespace pupsnes
