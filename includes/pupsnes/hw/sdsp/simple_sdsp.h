#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "pupsnes/hw/sdsp/sdsp.h"

namespace pupsnes {

// Fast, approximate S-DSP backend.
//
// PLACEHOLDER — all methods are stubs that compile but produce silence and
// ignore writes. Real implementation arrives alongside SPC700 work.
//
// Planned scope when fleshed out:
//   8 voices, BRR decode with linear interpolation, basic ADSR, sum-and-clip.
//   Explicitly out of scope: echo + FIR, noise, pitch modulation, gaussian
//   interpolation, KON/KOFF latency quirks.
class SimpleSdsp final : public Sdsp {
 public:
  SimpleSdsp(uint8_t* aram, std::size_t aram_size);
  ~SimpleSdsp() override = default;

  void Reset() override;
  [[nodiscard]] uint8_t ReadRegister(uint8_t index) const override;
  void WriteRegister(uint8_t index, uint8_t value) override;
  void StepSample(int16_t& out_left, int16_t& out_right) override;

  [[nodiscard]] SdspMode Mode() const override { return SdspMode::kSimple; }
  [[nodiscard]] std::string_view ModeName() const override { return "simple"; }
};

}  // namespace pupsnes
