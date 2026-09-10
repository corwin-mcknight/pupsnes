#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "pupsnes/hw/apu/sdsp.h"

namespace pupsnes {

// Approximate interpolation with the complete shared DSP pipeline: eight
// BRR voices, ADSR/GAIN, noise, pitch modulation, echo/FIR, and KON/KOFF timing.
// Linear interpolation replaces the hardware Gaussian filter.
class SimpleSdsp final : public Sdsp {
 public:
  SimpleSdsp(uint8_t* aram, std::size_t aram_size);
  ~SimpleSdsp() override = default;

  [[nodiscard]] SdspMode Mode() const override { return SdspMode::kSimple; }
  [[nodiscard]] std::string_view ModeName() const override { return "simple"; }
};

}  // namespace pupsnes
