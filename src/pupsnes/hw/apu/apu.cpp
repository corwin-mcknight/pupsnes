#include "pupsnes/hw/apu/apu.h"

#include <format>
#include <stdexcept>

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

void Apu::Reset() {
  // Deterministic cold-start policy; physical ARAM power-on contents vary.
  local_time_ = 0;
  clock_phase_ = 0;
  ram_.fill(0);
  input_ports_.fill(0);
  output_ports_.fill(0);
  ipl_enabled_ = true;
  dsp_address_ = 0;
  fault_.reset();
  cpu_.Reset();
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
  switch (address) {
    case 0xF0:
    case 0xF1:
    case 0xFA:
    case 0xFB:
    case 0xFC:
    case 0xFD:
    case 0xFE:
    case 0xFF: return 0;  // Write-only registers; disabled timer outputs.
    case 0xF2: return dsp_address_;
    case 0xF3: UnsupportedRegister(address, dsp_address_);
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
  switch (address) {
    case 0xF0:
      if ((cpu_.GetState().psw & 0x20U) == 0 && data != 0x0AU) UnsupportedRegister(address, data);
      break;
    case 0xF1:
      if ((data & 7U) != 0) UnsupportedRegister(address, data);
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
    case 0xF3: UnsupportedRegister(address, data);
    default: break;
  }
}

void Apu::UnsupportedRegister(uint16_t address, uint8_t data) {
  if (!fault_) {
    fault_ = std::format(
        "SPC700: unsupported APU register ${:04X} (value ${:02X}, PC ${:04X}); "
        "timers, DSP, and TEST modes are not implemented",
        address, data, cpu_.GetState().pc);
  }
  throw std::runtime_error(*fault_);
}

}  // namespace pupsnes
