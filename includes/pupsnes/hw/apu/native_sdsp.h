#pragma once

#include "pupsnes/hw/apu/stub_sdsp.h"

namespace pupsnes {

// Reserved for PupSNES's own synthesis implementation. Explicitly equivalent
// to Stub for now: silent, with no imported engine or synthesis fallback.
class NativeSdsp final : public StubSdsp {
 public:
  using StubSdsp::StubSdsp;
  [[nodiscard]] std::string_view ModeName() const override { return "native"; }
  [[nodiscard]] SdspBackend Backend() const override { return SdspBackend::kNative; }
};

}  // namespace pupsnes
