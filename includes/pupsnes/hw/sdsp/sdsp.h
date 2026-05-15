#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pupsnes {

// Selector for which S-DSP backend the APU constructs at Reset. Sourced from
// config at startup; debugger can mutate the selection but the swap only takes
// effect on the next Reset — the two backends do not share state and cannot
// be hot-swapped mid-run.
enum class SdspMode : uint8_t {
  kSimple = 0,    // Fast, approximate. No echo/noise/gaussian/pitch-mod/FIR.
  kAccurate = 1,  // Cycle-accurate, full feature set.
};

// Abstract S-DSP backend. Not a Device — the DSP is internal to the APU and
// not visible on the main 24-bit CPU bus. The SPC700 reaches it through its
// own $00F2 (address) / $00F3 (data) port pair; the APU translates those
// accesses into calls on this interface.
//
// Output contract: both backends produce signed 16-bit stereo at the native
// 32 kHz DSP sample rate. Resampling to host rate happens downstream so the
// choice of backend cannot affect game-observable timing.
//
// Test contract: the 128-byte register file's read-back state must be
// bit-identical between backends after the same input trace — that's what
// games actually observe. Sample output is allowed to differ (simple mode
// is approximate by design).
class Sdsp {
 public:
  // ARAM is the SPC700's 64KB main memory; the DSP reads BRR samples and
  // reads/writes the echo buffer through it. The pointer is non-owning;
  // the APU owns the storage and must outlive the Sdsp.
  Sdsp(uint8_t* aram, std::size_t aram_size) : aram_(aram), aram_size_(aram_size) {}
  virtual ~Sdsp() = default;

  Sdsp(const Sdsp&) = delete;
  Sdsp& operator=(const Sdsp&) = delete;

  // Power-on reset. Clears registers, voice state, envelopes. ARAM is the
  // SPC700's memory and is reset by the APU, not by us.
  virtual void Reset() = 0;

  // 128-byte DSP register file accessed by the SPC700 through $00F2/$00F3.
  // Index is masked to 7 bits by the caller; implementations may assume
  // index < 0x80.
  [[nodiscard]] virtual uint8_t ReadRegister(uint8_t index) const = 0;
  virtual void WriteRegister(uint8_t index, uint8_t value) = 0;

  // Advance the DSP by one 32 kHz sample period. The APU's clock driver
  // calls this every 32 SPC700 cycles. Output is signed 16-bit stereo at
  // 32 kHz. Both backends step on the same cadence so the SPC700 sees the
  // same register-file evolution regardless of which backend is active.
  virtual void StepSample(int16_t& out_left, int16_t& out_right) = 0;

  [[nodiscard]] virtual SdspMode Mode() const = 0;
  [[nodiscard]] virtual std::string_view ModeName() const = 0;

 protected:
  uint8_t* aram_;
  std::size_t aram_size_;
};

}  // namespace pupsnes
