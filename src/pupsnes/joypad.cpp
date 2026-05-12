#include "pupsnes/hw/joypad.h"

namespace pupsnes {

Joypad::Joypad(SNES* snes) : Device(snes) {}

void Joypad::Reset() {
  shift_register_ = 0;
  shift_count_ = 0;
  strobe_high_ = false;
}

void Joypad::SetButton(Button button, bool pressed) {
  const uint16_t mask = static_cast<uint16_t>(1U << static_cast<uint8_t>(button));
  if (pressed) {
    p1_state_ = static_cast<uint16_t>(p1_state_ | mask);
  } else {
    p1_state_ = static_cast<uint16_t>(p1_state_ & ~mask);
  }
}

bool Joypad::GetButton(Button button) const {
  const uint16_t mask = static_cast<uint16_t>(1U << static_cast<uint8_t>(button));
  return (p1_state_ & mask) != 0U;
}

void Joypad::WriteJoySer0(uint8_t data) {
  const bool new_strobe = (data & 0x01U) != 0U;
  // Strobe high re-loads the shift register from live state every cycle; the
  // 1 → 0 falling edge freezes that snapshot for the upcoming 16-shift
  // sequence. Both look the same to us — sample now and reset the counter.
  // Only a steady 0 → 0 write does nothing.
  if (new_strobe || strobe_high_) {
    shift_register_ = p1_state_;
    shift_count_ = 0;
  }
  strobe_high_ = new_strobe;
}

uint8_t Joypad::ReadJoySer0() {
  // While strobe is held high, $4016 reads return the current B-button bit
  // (the MSB of the live state) without consuming the shift register. Games
  // that just want the B-bit's level on demand use this; SNES reset detection
  // sometimes does.
  if (strobe_high_) {
    shift_register_ = p1_state_;
    shift_count_ = 0;
    return static_cast<uint8_t>((p1_state_ >> 15U) & 0x01U);
  }
  if (shift_count_ >= 16U) {
    // Open data line — real hardware reports 1 on a SNES standard controller
    // once the 16-bit window has been clocked out.
    return 0x01U;
  }
  const uint8_t bit = static_cast<uint8_t>((shift_register_ >> 15U) & 0x01U);
  shift_register_ = static_cast<uint16_t>(shift_register_ << 1U);
  ++shift_count_;
  return bit;
}

}  // namespace pupsnes
