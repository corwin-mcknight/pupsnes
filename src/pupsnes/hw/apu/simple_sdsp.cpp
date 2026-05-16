#include "pupsnes/hw/apu/simple_sdsp.h"

#include <cstddef>
#include <cstdint>

namespace pupsnes {

SimpleSdsp::SimpleSdsp(uint8_t* aram, std::size_t aram_size)
    : Sdsp(aram, aram_size) {}

void SimpleSdsp::Reset() {}

uint8_t SimpleSdsp::ReadRegister(uint8_t /*index*/) const {
  return 0;
}

void SimpleSdsp::WriteRegister(uint8_t /*index*/, uint8_t /*value*/) {}

void SimpleSdsp::StepSample(int16_t& out_left, int16_t& out_right) {
  out_left = 0;
  out_right = 0;
}

}  // namespace pupsnes
