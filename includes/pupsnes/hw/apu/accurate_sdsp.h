#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "pupsnes/hw/apu/sdsp.h"

namespace pupsnes {

// Cycle-accurate S-DSP backend.
//
// PLACEHOLDER — all methods are stubs that compile but produce silence and
// ignore writes. Real implementation arrives alongside SPC700 work.
//
// Planned scope when fleshed out:
//   Full 32 kHz sample loop with the per-cycle DSP state machine, 8 voices
//   with 4-tap gaussian interpolation, complete ADSR/gain envelope including
//   hardware quirks, echo buffer with 8-tap FIR, noise LFSR, pitch modulation,
//   KON/KOFF latency, BRR end-of-block and loop flags.
class AccurateSdsp final : public Sdsp {
 public:
  AccurateSdsp(uint8_t* aram, std::size_t aram_size);
  ~AccurateSdsp() override = default;

  void Reset() override;
  [[nodiscard]] uint8_t ReadRegister(uint8_t index) const override;
  void WriteRegister(uint8_t index, uint8_t value) override;
  void StepSample(int16_t& out_left, int16_t& out_right) override;

  [[nodiscard]] SdspMode Mode() const override { return SdspMode::kAccurate; }
  [[nodiscard]] std::string_view ModeName() const override { return "accurate"; }
};

}  // namespace pupsnes
