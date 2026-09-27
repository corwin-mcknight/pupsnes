#include "pupsnes/hw/input/joypad.h"

namespace pupsnes {

Joypad::Joypad(SNES& snes) : Device(snes) {}

void Joypad::Reset() {
  for (unsigned port = 0; port < 2; ++port) {
    shift_registers_[port] = 0;
    shift_counts_[port] = 0;
  }
  strobe_high_ = false;
}

void Joypad::SetButton(Button button, bool pressed, unsigned port) {
  const uint16_t mask = static_cast<uint16_t>(1U << static_cast<uint8_t>(button));
  if (pressed) {
    states_[port] = static_cast<uint16_t>(states_[port] | mask);
  } else {
    states_[port] = static_cast<uint16_t>(states_[port] & ~mask);
  }
}

bool Joypad::GetButton(Button button, unsigned port) const {
  const uint16_t mask = static_cast<uint16_t>(1U << static_cast<uint8_t>(button));
  return (states_[port] & mask) != 0U;
}

void Joypad::ReleaseAll(unsigned port) { states_[port] = 0; }

void Joypad::WriteJoySer0(uint8_t data) {
  const bool new_strobe = (data & 0x01U) != 0U;
  // Strobe high re-loads the shift register from live state every cycle; the
  // 1 → 0 falling edge freezes that snapshot for the upcoming 16-shift
  // sequence. Both look the same to us — sample now and reset the counter.
  // Only a steady 0 → 0 write does nothing.
  if (new_strobe || strobe_high_) {
    for (unsigned port = 0; port < 2; ++port) {
      shift_registers_[port] = states_[port];
      shift_counts_[port] = 0;
    }
  }
  strobe_high_ = new_strobe;
}

uint8_t Joypad::ReadSerial(unsigned port) {
  // While strobe is held high, either port returns the current B-button bit
  // (the MSB of the live state) without consuming the shift register. Games
  // that just want the B-bit's level on demand use this; SNES reset detection
  // sometimes does.
  if (strobe_high_) {
    shift_registers_[port] = states_[port];
    shift_counts_[port] = 0;
    return static_cast<uint8_t>((states_[port] >> 15U) & 0x01U);
  }
  if (shift_counts_[port] >= 16U) {
    // Open data line — real hardware reports 1 on a SNES standard controller
    // once the 16-bit window has been clocked out.
    return 0x01U;
  }
  const uint8_t bit = static_cast<uint8_t>((shift_registers_[port] >> 15U) & 0x01U);
  shift_registers_[port] = static_cast<uint16_t>(shift_registers_[port] << 1U);
  ++shift_counts_[port];
  return bit;
}

uint8_t Joypad::ReadJoySer0() { return ReadSerial(0); }
uint8_t Joypad::ReadJoySer1() { return ReadSerial(1); }

}  // namespace pupsnes
