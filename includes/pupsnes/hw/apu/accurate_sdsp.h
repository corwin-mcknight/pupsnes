#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "pupsnes/hw/apu/sdsp.h"

namespace pupsnes {

// The snes_spc DSP pipeline with hardware Gaussian interpolation, all eight
// BRR voices, ADSR/GAIN, noise, pitch modulation, echo/FIR, and KON/KOFF timing.
// Register and ARAM accesses execute on their individual SPC clock edges.
class AccurateSdsp final : public Sdsp {
 public:
  AccurateSdsp(uint8_t* aram, std::size_t aram_size);
  ~AccurateSdsp() override = default;

  [[nodiscard]] SdspMode Mode() const override { return SdspMode::kAccurate; }
  [[nodiscard]] std::string_view ModeName() const override { return "accurate"; }
};

}  // namespace pupsnes
