#include "pupsnes/hw/apu/apu.h"

#include <format>
#include <stdexcept>

#include "pupsnes/core/snes.h"
#include "pupsnes/hw/apu/accurate_sdsp.h"
#include "pupsnes/hw/apu/simple_sdsp.h"

namespace pupsnes {
namespace {

// S-SMP's 64-byte IPL program (including the reset vector at $FFFE).
// Disassembly/reference: https://github.com/gilligan/snesdev/blob/master/docs/spc700.txt
constexpr std::array<uint8_t, 64> kIplRom = {
    0xCD, 0xEF, 0xBD, 0xE8, 0x00, 0xC6, 0x1D, 0xD0, 0xFC, 0x8F, 0xAA, 0xF4, 0x8F, 0xBB, 0xF5, 0x78,
    0xCC, 0xF4, 0xD0, 0xFB, 0x2F, 0x19, 0xEB, 0xF4, 0xD0, 0xFC, 0x7E, 0xF4, 0xD0, 0x0B, 0xE4, 0xF5,
    0xCB, 0xF4, 0xD7, 0x00, 0xFC, 0xD0, 0xF3, 0xAB, 0x01, 0x10, 0xEF, 0x7E, 0xF4, 0x10, 0xEB, 0xBA,
    0xF6, 0xDA, 0x00, 0xBA, 0xF4, 0xC4, 0xF4, 0xDD, 0x5D, 0xD0, 0xDB, 0x1F, 0x00, 0x00, 0xC0, 0xFF,
};

}  // namespace

Apu::Apu(SNES& snes) : Device(snes), cpu_(*this) { Reset(); }

void Apu::Reset(SdspMode mode) {
  if (!dsp_ || dsp_->Mode() != mode) {
    switch (mode) {
      case SdspMode::kSimple: dsp_ = std::make_unique<SimpleSdsp>(ram_.data(), ram_.size()); break;
      case SdspMode::kAccurate: dsp_ = std::make_unique<AccurateSdsp>(ram_.data(), ram_.size()); break;
      default: throw std::invalid_argument("Invalid S-DSP mode");
    }
  }
  // Deterministic cold-start policy; physical ARAM power-on contents vary.
  local_time_ = 0;
  clock_phase_ = 0;
  ram_.fill(0);
  input_ports_.fill(0);
  output_ports_.fill(0);
  auxiliary_ports_.fill(0);
  timers_.fill({});
  dsp_clock_phase_ = 0;
  dsp_sample_count_ = 0;
  dsp_->Reset();
  ipl_enabled_ = true;
  dsp_address_ = 0;
  fault_.reset();
  cpu_.Reset();
}

void Apu::StepHardware() {
  // Complete peripheral edges before this cycle's SPC bus access. This matches
  // the S-SMP wait/read/write ordering in ares/sfc/smp/{timing,memory}.cpp.
  dsp_clock_phase_ = static_cast<uint8_t>((dsp_clock_phase_ + 1U) & 31U);
  int16_t left = 0;
  int16_t right = 0;
  if (dsp_->TickCycle(left, right)) {
    ++dsp_sample_count_;
    snes_->FireAudioSample(left, right);
  }

  // The first divider is free-running even when its timer is disabled. The
  // programmable counter compares after incrementing, so target zero is 256.
  // Reference: https://github.com/gilligan/snesdev/blob/master/docs/spc700.txt
  constexpr std::array<uint8_t, 3> kPeriods = {128, 128, 16};
  for (std::size_t index = 0; index < timers_.size(); ++index) {
    auto& timer = timers_[index];
    if (++timer.divider != kPeriods[index]) continue;
    timer.divider = 0;
    if (!timer.enabled) continue;
    timer.counter = static_cast<uint8_t>(timer.counter + 1U);
    if (timer.counter != timer.target) continue;
    timer.counter = 0;
    timer.output = static_cast<uint8_t>((timer.output + 1U) & 0x0FU);
  }
}

void Apu::CheckFault() {
  const auto& state = cpu_.GetState();
  if (!fault_ && state.faulted) {
    fault_ = std::format("SPC700: core fault at opcode ${:02X}, address ${:04X}", state.fault_opcode, state.fault_pc);
  }
  if (fault_) throw std::runtime_error(*fault_);
}

void Apu::CatchUpTo(TimeMasterT target) {
  CheckFault();
  while (local_time_ < target) {
    // Round each APU edge up to its first representable master cycle. The
    // remainder preserves its real fractional position rather than drifting.
    const TimeMasterT until_edge = (kClockDenominator - clock_phase_ + kClockNumerator - 1) / kClockNumerator;
    const TimeMasterT remaining = target - local_time_;
    if (remaining < until_edge) {
      clock_phase_ += remaining * kClockNumerator;
      local_time_ = target;
      return;
    }
    local_time_ += until_edge;
    clock_phase_ += until_edge * kClockNumerator;
    clock_phase_ -= kClockDenominator;
    StepHardware();
    cpu_.TickCycle();
    CheckFault();
  }
}

MmioReadResult Apu::ReadRegister(uint32_t offset, TimeMasterT current_time) {
  CatchUpTo(current_time);
  return {GetPort(offset), 0xFF};
}

void Apu::WriteRegister(uint32_t offset, uint8_t data, TimeMasterT current_time) {
  // All completed APU edges at this time precede the CPU latch change.
  // Input/output latches are independent; writes never acknowledge themselves.
  CatchUpTo(current_time);
  input_ports_[offset & 3U] = data;
}

std::optional<uint8_t> Apu::HandleDebugRead(uint32_t offset) const { return GetPort(offset); }

bool Apu::HandleDebugWrite(uint32_t /*offset*/, uint8_t /*data*/) { return false; }

uint8_t Apu::Read(uint16_t address) {
  if (address >= 0xFFC0U && ipl_enabled_) return kIplRom[address - 0xFFC0U];
  if (address >= 0xF4U && address <= 0xF7U) return input_ports_[address & 3U];
  if (address == 0xF8U || address == 0xF9U) return auxiliary_ports_[address - 0xF8U];
  switch (address) {
    case 0xF0:
    case 0xF1:
    case 0xFA:
    case 0xFB:
    case 0xFC: return 0;  // Write-only registers.
    case 0xFD:
    case 0xFE:
    case 0xFF: {
      auto& timer = timers_[address - 0xFDU];
      const uint8_t result = timer.output;
      timer.output = 0;
      return result;
    }
    case 0xF2: return dsp_address_;
    case 0xF3: return dsp_->ReadRegister(dsp_address_ & 0x7FU);
    default: return ram_[address];
  }
}

void Apu::Write(uint16_t address, uint8_t data) {
  // Writes always reach underlying ARAM, including below the IPL and I/O.
  // Non-default TEST RAM modes are deliberately rejected below.
  ram_[address] = data;
  if (address >= 0xF4U && address <= 0xF7U) {
    output_ports_[address & 3U] = data;
    return;
  }
  if (address == 0xF8U || address == 0xF9U) {
    // DSP echo writes reach ARAM directly and must not replace these latches.
    auxiliary_ports_[address - 0xF8U] = data;
    return;
  }
  switch (address) {
    case 0xF0:
      if ((cpu_.GetState().psw & 0x20U) == 0 && data != 0x0AU) UnsupportedRegister(address, data);
      break;
    case 0xF1:
      for (std::size_t index = 0; index < timers_.size(); ++index) {
        auto& timer = timers_[index];
        const bool enabled = (data & (1U << index)) != 0;
        if (enabled && !timer.enabled) {
          timer.counter = 0;
          timer.output = 0;
        }
        timer.enabled = enabled;
      }
      if ((data & 0x10U) != 0) {
        input_ports_[0] = 0;
        input_ports_[1] = 0;
      }
      if ((data & 0x20U) != 0) {
        input_ports_[2] = 0;
        input_ports_[3] = 0;
      }
      ipl_enabled_ = (data & 0x80U) != 0;
      break;
    case 0xF2: dsp_address_ = data; break;
    case 0xF3:
      if ((dsp_address_ & 0x80U) == 0) dsp_->WriteRegister(dsp_address_, data);
      break;
    case 0xFA:
    case 0xFB:
    case 0xFC: timers_[address - 0xFAU].target = data; break;
    // This includes $FD-$FF: writing an output counter affects only ARAM.
    default: break;
  }
}

void Apu::UnsupportedRegister(uint16_t address, uint8_t data) {
  if (!fault_) {
    fault_ = std::format(
        "SPC700: unsupported APU register ${:04X} (value ${:02X}, PC ${:04X}); "
        "non-default TEST modes are not implemented",
        address, data, cpu_.GetState().pc);
  }
  throw std::runtime_error(*fault_);
}

}  // namespace pupsnes
