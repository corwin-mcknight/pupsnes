#include "pupsnes/hw/apu/stub_sdsp.h"

#include <cassert>
#include <stdexcept>

namespace pupsnes {

StubSdsp::StubSdsp(uint8_t* aram, std::size_t aram_size, SdspMode mode) : mode_(mode) {
  if (aram == nullptr || aram_size != 65536) throw std::invalid_argument("S-DSP requires 64 KiB of ARAM");
  if (mode != SdspMode::kSimple && mode != SdspMode::kAccurate) throw std::invalid_argument("Invalid interpolation");
  StubSdsp::Reset();
}

void StubSdsp::Reset() {
  std::array<uint8_t, kRegisterCount> registers{};
  registers[0x6C] = 0xE0;
  StubSdsp::LoadRegisters(registers);
}

void StubSdsp::LoadRegisters(const std::array<uint8_t, kRegisterCount>& registers) {
  registers_ = registers;
  phase_ = 0;
}

uint8_t StubSdsp::ReadRegister(uint8_t index) const {
  assert(index < kRegisterCount);
  return registers_[index];
}

void StubSdsp::WriteRegister(uint8_t index, uint8_t value) {
  assert(index < kRegisterCount);
  registers_[index] = index == 0x7C ? 0 : value;
}

bool StubSdsp::TickCycle(int16_t& out_left, int16_t& out_right) {
  if (++phase_ != 32) return false;
  phase_ = 0;
  // ENVX/OUTX describe silent voices. No fabricated voice-end events.
  for (std::size_t voice = 0; voice < 8; ++voice) {
    registers_[voice * 16 + 8] = 0;
    registers_[voice * 16 + 9] = 0;
  }
  out_left = out_right = 0;
  return true;
}

}  // namespace pupsnes
