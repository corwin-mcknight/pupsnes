#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace pupsnes {

// Interpolation selection for the third-party backend. Numeric values retain
// compatibility with saved sdsp_mode preferences. Applied on reset.
enum class SdspMode : uint8_t {
  kSimple = 0,    // Shared DSP pipeline with approximate linear interpolation.
  kAccurate = 1,  // Hardware Gaussian interpolation and DSP cycle sequencing.
};

enum class SdspBackend : uint8_t { kStub = 0, kThirdParty = 1, kNative = 2 };

[[nodiscard]] const char* SdspBackendName(SdspBackend backend);

// Abstract S-DSP backend. Not a Device — the DSP is internal to the APU and
// not visible on the main 24-bit CPU bus. The SPC700 reaches it through its
// own $00F2 (address) / $00F3 (data) port pair; the APU translates those
// accesses into calls on this interface.
//
// The third-party backend runs snes_spc. Stub and the initial Native placeholder
// maintain a register bank and sample cadence but perform no synthesis or echo.
//
// Output contract: signed 16-bit stereo at the native 32 kHz sample rate.
// Future host resampling belongs downstream of this interface.
//
// CPU register writes have the same semantics in both modes. Interpolation
// changes OUTX, pitch modulation, and echo samples, so resulting status and
// ARAM contents can differ between modes while event sequencing is shared.
class Sdsp {
 public:
  static constexpr std::size_t kRegisterCount = 0x80;

  Sdsp() = default;
  virtual ~Sdsp() = default;

  Sdsp(const Sdsp&) = delete;
  Sdsp& operator=(const Sdsp&) = delete;

  // Deterministic cold reset: all registers zero except FLG=$E0 (reset,
  // mute, echo-write disable). Physical power-on contents of other
  // registers are unspecified. ARAM belongs to the APU and is preserved.
  virtual void Reset() = 0;

  // Initialize from an SPC register image, without register-write side effects.
  virtual void LoadRegisters(const std::array<uint8_t, kRegisterCount>& registers) = 0;

  // 128-byte DSP register file accessed by the SPC700 through $00F2/$00F3.
  // Index is masked to 7 bits by the caller; implementations may assume
  // index < 0x80.
  // Writes retain all eight bits, including otherwise unused bits and
  // ENVX/OUTX until overwritten by the voice pipeline. Writing any value
  // to ENDX clears its entire byte.
  [[nodiscard]] virtual uint8_t ReadRegister(uint8_t index) const = 0;
  virtual void WriteRegister(uint8_t index, uint8_t value) = 0;

  // Advance one SPC clock, including that clock's register and ARAM effects.
  // Return one stereo sample on each 32nd call after Reset; otherwise leave
  // the output arguments unchanged. The internal DAC result is held until
  // this delivery boundary without delaying hardware state evolution.
  virtual bool TickCycle(int16_t& out_left, int16_t& out_right) = 0;

  // Convenience for standalone synthesis: advance exactly 32 clocks and
  // return the one sample delivered during that interval.
  virtual void StepSample(int16_t& out_left, int16_t& out_right);

  [[nodiscard]] virtual SdspMode Mode() const = 0;
  [[nodiscard]] virtual std::string_view ModeName() const = 0;
  [[nodiscard]] virtual SdspBackend Backend() const = 0;
};

// ARAM is the SPC700's 64 KiB memory, owned by the APU. Backends may access it
// for BRR and echo; the APU must outlive the returned DSP.
[[nodiscard]] std::unique_ptr<Sdsp> MakeSdsp(SdspBackend backend, SdspMode mode, uint8_t* aram, std::size_t aram_size);

}  // namespace pupsnes
