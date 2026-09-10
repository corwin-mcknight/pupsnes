#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace pupsnes {

// Selector for which S-DSP backend the APU constructs at Reset. Sourced from
// config at startup; debugger can mutate the selection but the swap only takes
// effect on the next Reset — the two backends do not share state and cannot
// be hot-swapped mid-run.
enum class SdspMode : uint8_t {
  kSimple = 0,    // Shared DSP pipeline with approximate linear interpolation.
  kAccurate = 1,  // Hardware Gaussian interpolation and DSP cycle sequencing.
};

// Abstract S-DSP backend. Not a Device — the DSP is internal to the APU and
// not visible on the main 24-bit CPU bus. The SPC700 reaches it through its
// own $00F2 (address) / $00F3 (data) port pair; the APU translates those
// accesses into calls on this interface.
//
// Both backends run the snes_spc voice, BRR, envelope, noise, pitch modulation,
// and echo/FIR pipeline one SPC clock at a time. Simple uses linear sample
// interpolation; Accurate preserves the hardware Gaussian filter.
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

  // ARAM is the SPC700's 64KB main memory; the DSP reads BRR samples and
  // reads/writes the echo buffer through it. The pointer is non-owning;
  // the APU owns the storage and must outlive the Sdsp.
  Sdsp(uint8_t* aram, std::size_t aram_size, SdspMode mode);
  virtual ~Sdsp();

  Sdsp(const Sdsp&) = delete;
  Sdsp& operator=(const Sdsp&) = delete;

  // Deterministic cold reset: all registers zero except FLG=$E0 (reset,
  // mute, echo-write disable). Physical power-on contents of other
  // registers are unspecified. ARAM belongs to the APU and is preserved.
  virtual void Reset();

  // 128-byte DSP register file accessed by the SPC700 through $00F2/$00F3.
  // Index is masked to 7 bits by the caller; implementations may assume
  // index < 0x80.
  // Writes retain all eight bits, including otherwise unused bits and
  // ENVX/OUTX until overwritten by the voice pipeline. Writing any value
  // to ENDX clears its entire byte.
  [[nodiscard]] virtual uint8_t ReadRegister(uint8_t index) const;
  virtual void WriteRegister(uint8_t index, uint8_t value);

  // Advance one SPC clock, including that clock's register and ARAM effects.
  // Return one stereo sample on each 32nd call after Reset; otherwise leave
  // the output arguments unchanged. The internal DAC result is held until
  // this delivery boundary without delaying hardware state evolution.
  virtual bool TickCycle(int16_t& out_left, int16_t& out_right);

  // Convenience for standalone synthesis: advance exactly 32 clocks and
  // return the one sample delivered during that interval.
  virtual void StepSample(int16_t& out_left, int16_t& out_right);

  [[nodiscard]] virtual SdspMode Mode() const = 0;
  [[nodiscard]] virtual std::string_view ModeName() const = 0;

 private:
  struct Engine;
  std::unique_ptr<Engine> engine_;
};

}  // namespace pupsnes
