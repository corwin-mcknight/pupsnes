#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "pupsnes/core/device.h"
#include "pupsnes/hw/apu/sdsp.h"
#include "pupsnes/hw/apu/spc700.h"

namespace pupsnes {

// S-SMP with all SPC700 opcodes, three timers, and the S-DSP register interface.
// Non-default TEST modes stop with a diagnostic.
class Apu final : public Device, public Spc700Bus {
 public:
  struct TimerState {
    uint8_t divider = 0;
    uint8_t counter = 0;
    uint8_t target = 0;
    uint8_t output = 0;
    bool enabled = false;

    bool operator==(const TimerState&) const = default;
  };

  static constexpr uint32_t kPortBase = 0x2140U;
  static constexpr uint32_t kPortEnd = 0x2180U;
  static constexpr std::size_t kRamSize = 65536;

  // Nominal NTSC master clock = 236250000/11 Hz; SPC clock = 1024000 Hz.
  // Their reduced ratio is exact and keeps fractional cycles across slices.
  static constexpr TimeMasterT kClockNumerator = 5632;
  static constexpr TimeMasterT kClockDenominator = 118125;

  explicit Apu(SNES& snes);
  [[nodiscard]] const char* DeviceName() const override { return "APU (SPC700)"; }
  void Reset(SdspMode mode = SdspMode::kAccurate);
  void CatchUpTo(TimeMasterT target) override;
  [[nodiscard]] MmioReadResult ReadRegister(uint32_t offset, TimeMasterT current_time) override;
  void WriteRegister(uint32_t offset, uint8_t data, TimeMasterT current_time) override;
  [[nodiscard]] std::optional<uint8_t> HandleDebugRead(uint32_t offset) const override;
  bool HandleDebugWrite(uint32_t offset, uint8_t data) override;

  // SPC bus, also usable by focused device tests. These have no clock side
  // effects: the core invokes one access at its corresponding cycle edge.
  [[nodiscard]] uint8_t Read(uint16_t address) override;
  void Write(uint16_t address, uint8_t data) override;
  [[nodiscard]] uint8_t PeekRam(uint16_t address) const { return ram_[address]; }
  [[nodiscard]] const std::array<uint8_t, kRamSize>& GetRam() const { return ram_; }
  [[nodiscard]] uint8_t GetPort(std::size_t port) const { return output_ports_[port & 3U]; }
  [[nodiscard]] uint8_t GetInputPort(std::size_t port) const { return input_ports_[port & 3U]; }
  [[nodiscard]] Spc700& GetCpu() { return cpu_; }
  [[nodiscard]] const Spc700& GetCpu() const { return cpu_; }
  [[nodiscard]] TimeMasterT GetClockPhase() const { return clock_phase_; }
  [[nodiscard]] const TimerState& GetTimerState(std::size_t timer) const { return timers_.at(timer); }
  [[nodiscard]] const Sdsp& GetDsp() const { return *dsp_; }
  [[nodiscard]] uint8_t GetDspClockPhase() const { return dsp_clock_phase_; }
  [[nodiscard]] uint64_t GetDspSampleCount() const { return dsp_sample_count_; }
  [[nodiscard]] const std::optional<std::string>& GetFault() const { return fault_; }

 private:
  [[noreturn]] void UnsupportedRegister(uint16_t address, uint8_t data);
  void CheckFault();
  void StepHardware();

  std::array<uint8_t, kRamSize> ram_{};
  std::array<uint8_t, 4> input_ports_{};
  std::array<uint8_t, 4> output_ports_{};
  std::array<uint8_t, 2> auxiliary_ports_{};
  Spc700 cpu_;
  std::array<TimerState, 3> timers_{};
  std::unique_ptr<Sdsp> dsp_;
  TimeMasterT clock_phase_ = 0;
  uint64_t dsp_sample_count_ = 0;
  uint8_t dsp_clock_phase_ = 0;
  bool ipl_enabled_ = true;
  uint8_t dsp_address_ = 0;
  std::optional<std::string> fault_;
};

}  // namespace pupsnes
