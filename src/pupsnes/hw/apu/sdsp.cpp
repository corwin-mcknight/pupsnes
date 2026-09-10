#include "pupsnes/hw/apu/sdsp.h"

#include <SPC_DSP.h>

#include <array>
#include <cassert>
#include <stdexcept>

namespace pupsnes {

struct Sdsp::Engine {
  SPC_DSP dsp{};
  // Four scalar slots keep upstream's output pointer inside this buffer after
  // writing one stereo frame, avoiding its overflow-to-scratch-buffer path.
  std::array<SPC_DSP::sample_t, 4> output{};
  uint8_t phase = 0;
};

Sdsp::Sdsp(uint8_t* aram, std::size_t aram_size, SdspMode mode) {
  if (aram == nullptr || aram_size != 65536) throw std::invalid_argument("S-DSP requires 64 KiB of ARAM");
  engine_ = std::make_unique<Engine>();
  engine_->dsp.init(aram);
  engine_->dsp.set_linear_interpolation(mode == SdspMode::kSimple);
  Sdsp::Reset();
}

Sdsp::~Sdsp() = default;

void Sdsp::Reset() {
  // Preserve the APU's deterministic cold seed rather than upstream's
  // example snapshot of unspecified physical power-on register contents.
  std::array<uint8_t, kRegisterCount> registers{};
  registers[0x6C] = 0xE0;
  engine_->dsp.load(registers.data());
  engine_->phase = 0;
  engine_->output.fill(0);
  engine_->dsp.set_output(engine_->output.data(), static_cast<int>(engine_->output.size()));
}

uint8_t Sdsp::ReadRegister(uint8_t index) const {
  assert(index < kRegisterCount);
  return static_cast<uint8_t>(engine_->dsp.read(index));
}

void Sdsp::WriteRegister(uint8_t index, uint8_t value) {
  assert(index < kRegisterCount);
  engine_->dsp.write(index, value);
}

bool Sdsp::TickCycle(int16_t& out_left, int16_t& out_right) {
  engine_->dsp.run(1);
  if (++engine_->phase != 32) return false;
  engine_->phase = 0;
  assert(engine_->dsp.sample_count() == 2);
  out_left = engine_->output[0];
  out_right = engine_->output[1];
  engine_->dsp.set_output(engine_->output.data(), static_cast<int>(engine_->output.size()));
  return true;
}

void Sdsp::StepSample(int16_t& out_left, int16_t& out_right) {
  for (unsigned cycle = 0; cycle < 32; ++cycle) static_cast<void>(TickCycle(out_left, out_right));
}

}  // namespace pupsnes
