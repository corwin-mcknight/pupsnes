#include "pupsnes/hw/apu/accurate_sdsp.h"

#include <cstddef>
#include <cstdint>

namespace pupsnes {

AccurateSdsp::AccurateSdsp(uint8_t* aram, std::size_t aram_size)
    : Sdsp(aram, aram_size) {}

void AccurateSdsp::Reset() {}

uint8_t AccurateSdsp::ReadRegister(uint8_t /*index*/) const {
  return 0;
}

void AccurateSdsp::WriteRegister(uint8_t /*index*/, uint8_t /*value*/) {}

void AccurateSdsp::StepSample(int16_t& out_left, int16_t& out_right) {
  out_left = 0;
  out_right = 0;
}

}  // namespace pupsnes
