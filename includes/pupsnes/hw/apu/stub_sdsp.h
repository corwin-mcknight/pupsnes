#pragma once

#include "pupsnes/hw/apu/sdsp.h"

namespace pupsnes {

// Silent register-level substitute. The real SPC700, timers, and IPL continue
// running in the APU. No voices, dynamic status, BRR, or ARAM echo writes.
class StubSdsp : public Sdsp {
 public:
  StubSdsp(uint8_t* aram, std::size_t aram_size, SdspMode mode = SdspMode::kAccurate);
  void Reset() override;
  void LoadRegisters(const std::array<uint8_t, kRegisterCount>& registers) override;
  [[nodiscard]] uint8_t ReadRegister(uint8_t index) const override;
  void WriteRegister(uint8_t index, uint8_t value) override;
  bool TickCycle(int16_t& out_left, int16_t& out_right) override;
  // Retain the requested interpolation preference; it has no effect here.
  [[nodiscard]] SdspMode Mode() const override { return mode_; }
  [[nodiscard]] std::string_view ModeName() const override { return "stub"; }
  [[nodiscard]] SdspBackend Backend() const override { return SdspBackend::kStub; }

 private:
  std::array<uint8_t, kRegisterCount> registers_{};
  uint8_t phase_ = 0;
  SdspMode mode_;
};

}  // namespace pupsnes
