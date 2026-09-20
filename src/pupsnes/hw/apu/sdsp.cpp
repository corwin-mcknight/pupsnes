#include "pupsnes/hw/apu/sdsp.h"

#include <stdexcept>

#include "pupsnes/hw/apu/native_sdsp.h"
#include "pupsnes/hw/apu/stub_sdsp.h"
#include "pupsnes/hw/apu/third_party_sdsp.h"

namespace pupsnes {

const char* SdspBackendName(SdspBackend backend) {
  switch (backend) {
    case SdspBackend::kStub: return "Stub";
    case SdspBackend::kThirdParty: return "Third-party";
    case SdspBackend::kNative: return "Native";
  }
  return "Unknown";
}

std::unique_ptr<Sdsp> MakeSdsp(SdspBackend backend, SdspMode mode, uint8_t* aram, std::size_t aram_size) {
  switch (backend) {
    case SdspBackend::kStub: return std::make_unique<StubSdsp>(aram, aram_size, mode);
    case SdspBackend::kThirdParty: return std::make_unique<ThirdPartySdsp>(aram, aram_size, mode);
    case SdspBackend::kNative: return std::make_unique<NativeSdsp>(aram, aram_size, mode);
  }
  throw std::invalid_argument("Invalid S-DSP backend");
}

void Sdsp::StepSample(int16_t& out_left, int16_t& out_right) {
  for (unsigned cycle = 0; cycle < 32; ++cycle) static_cast<void>(TickCycle(out_left, out_right));
}

}  // namespace pupsnes
