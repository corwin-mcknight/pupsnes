#include "pupsnes/hw/apu/third_party_sdsp.h"

#include <SPC_DSP.h>

#include <array>
#include <cassert>
#include <memory>
#include <stdexcept>

namespace pupsnes {

struct ThirdPartySdsp::Engine {
  SPC_DSP dsp{};
  // Four scalar slots keep upstream's output pointer inside this buffer after
  // writing one stereo frame, avoiding its overflow-to-scratch-buffer path.
  std::array<SPC_DSP::sample_t, 4> output{};
  uint8_t phase = 0;
};

ThirdPartySdsp::ThirdPartySdsp(uint8_t* aram, std::size_t aram_size, SdspMode mode) : mode_(mode) {
  if (aram == nullptr || aram_size != 65536) throw std::invalid_argument("S-DSP requires 64 KiB of ARAM");
  if (mode != SdspMode::kSimple && mode != SdspMode::kAccurate) throw std::invalid_argument("Invalid interpolation");
  engine_ = std::make_unique<Engine>();
  engine_->dsp.init(aram);
  engine_->dsp.set_linear_interpolation(mode == SdspMode::kSimple);
  ThirdPartySdsp::Reset();
}

ThirdPartySdsp::~ThirdPartySdsp() = default;

void ThirdPartySdsp::Reset() {
  // Preserve the APU's deterministic cold seed rather than upstream's
  // example snapshot of unspecified physical power-on register contents.
  std::array<uint8_t, kRegisterCount> registers{};
  registers[0x6C] = 0xE0;
  ThirdPartySdsp::LoadRegisters(registers);
}

void ThirdPartySdsp::LoadRegisters(const std::array<uint8_t, kRegisterCount>& registers) {
  engine_->dsp.load(registers.data());
  engine_->phase = 0;
  engine_->output.fill(0);
  engine_->dsp.set_output(engine_->output.data(), static_cast<int>(engine_->output.size()));
}

uint8_t ThirdPartySdsp::ReadRegister(uint8_t index) const {
  assert(index < kRegisterCount);
  return static_cast<uint8_t>(engine_->dsp.read(index));
}

void ThirdPartySdsp::WriteRegister(uint8_t index, uint8_t value) {
  assert(index < kRegisterCount);
  engine_->dsp.write(index, value);
}

bool ThirdPartySdsp::TickCycle(int16_t& out_left, int16_t& out_right) {
  engine_->dsp.run(1);
  if (++engine_->phase != 32) return false;
  engine_->phase = 0;
  assert(engine_->dsp.sample_count() == 2);
  out_left = engine_->output[0];
  out_right = engine_->output[1];
  engine_->dsp.set_output(engine_->output.data(), static_cast<int>(engine_->output.size()));
  return true;
}

}  // namespace pupsnes
