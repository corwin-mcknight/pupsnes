#pragma once

#include "pupsnes/hw/apu/sdsp.h"

namespace pupsnes {

// All imported DSP state lives here, outside the common interface.
class ThirdPartySdsp : public Sdsp {
 public:
  ThirdPartySdsp(uint8_t* aram, std::size_t aram_size, SdspMode mode);
  ~ThirdPartySdsp() override;
  void Reset() override;
  void LoadRegisters(const std::array<uint8_t, kRegisterCount>& registers) override;
  [[nodiscard]] uint8_t ReadRegister(uint8_t index) const override;
  void WriteRegister(uint8_t index, uint8_t value) override;
  bool TickCycle(int16_t& out_left, int16_t& out_right) override;
  [[nodiscard]] SdspMode Mode() const override { return mode_; }
  [[nodiscard]] std::string_view ModeName() const override {
    return mode_ == SdspMode::kSimple ? "simple" : "accurate";
  }
  [[nodiscard]] SdspBackend Backend() const override { return SdspBackend::kThirdParty; }

 private:
  struct Engine;
  std::unique_ptr<Engine> engine_;
  SdspMode mode_;
};

}  // namespace pupsnes
