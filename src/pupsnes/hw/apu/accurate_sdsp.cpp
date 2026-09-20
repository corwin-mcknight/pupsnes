#include "pupsnes/hw/apu/accurate_sdsp.h"

#include <cstddef>
#include <cstdint>

namespace pupsnes {

AccurateSdsp::AccurateSdsp(uint8_t* aram, std::size_t aram_size)
    : ThirdPartySdsp(aram, aram_size, SdspMode::kAccurate) {}

}  // namespace pupsnes
