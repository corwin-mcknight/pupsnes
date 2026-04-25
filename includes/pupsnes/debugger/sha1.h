#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace pupsnes::debugger {

using Sha1Digest = std::array<uint8_t, 20>;

// Reference SHA-1 per RFC 3174. Not performance-critical — used only to
// hash a ROM image at load time (≤ 4 MiB, runs once). Not for cryptographic
// authentication; used here solely to anchor a trace file to its ROM so
// the comparison tool can refuse mismatched inputs.
class Sha1 {
 public:
  Sha1();

  void Update(const uint8_t* data, std::size_t len);
  [[nodiscard]] Sha1Digest Finalize();

 private:
  void ProcessBlock();

  uint32_t h_[5];
  uint8_t block_[64];
  std::size_t block_used_ = 0;
  uint64_t total_bits_ = 0;
};

}  // namespace pupsnes::debugger
