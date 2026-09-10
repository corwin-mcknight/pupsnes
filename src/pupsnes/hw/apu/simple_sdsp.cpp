#include "pupsnes/hw/apu/simple_sdsp.h"

#include <cstddef>
#include <cstdint>

namespace pupsnes {

SimpleSdsp::SimpleSdsp(uint8_t* aram, std::size_t aram_size) : Sdsp(aram, aram_size, SdspMode::kSimple) {}

}  // namespace pupsnes
