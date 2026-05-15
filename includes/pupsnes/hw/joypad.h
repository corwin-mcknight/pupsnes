#pragma once

#include <cstdint>

#include "pupsnes/hw/device.h"

namespace pupsnes {

// SNES standard controller, port 1 only.
//
// Holds the 16-bit auto-joypad word for P1 and implements both access paths a
// game might use:
//
//   * Auto-joypad read ($4218 = JOY1L low byte, $4219 = JOY1H high byte). v1
//     returns the current button state on demand — no $4200.0 gate, no
//     VBlank-edge latching, no 4224-mcyc busy window. Good enough for a debug
//     controller window where the user holds buttons for whole frames at a
//     time.
//   * Manual serial read ($4016 strobe + shift). Standard sequence: write 1
//     then 0 to $4016 to latch, then read $4016 sixteen times to clock out
//     bits MSB-first (B first). Reads past the 16-bit window return 1, matching
//     the open data line on real hardware.
//
// Bit layout (matches the auto-joypad word the hardware returns):
//
//   15 B   14 Y   13 Select  12 Start
//   11 Up  10 Down 9 Left     8 Right
//    7 A    6 X    5 L         4 R
//    3-0 controller-type ID (0 for std)
class Joypad : public Device {
 public:
  enum class Button : uint8_t {
    kB = 15,
    kY = 14,
    kSelect = 13,
    kStart = 12,
    kUp = 11,
    kDown = 10,
    kLeft = 9,
    kRight = 8,
    kA = 7,
    kX = 6,
    kL = 5,
    kR = 4,
  };

  explicit Joypad(SNES* snes);
  ~Joypad() override = default;

  [[nodiscard]] const char* DeviceName() const override { return "Joypad"; }

  // Reset clears the shift register, strobe latch, and shift counter so a
  // soft reset doesn't leave a partially-clocked manual read in flight. The
  // user-driven button state survives — the UI owns it and the user is the
  // source of truth, not the machine.
  void Reset();

  // Frontend API. The UI panel calls these.
  void SetButton(Button button, bool pressed);
  [[nodiscard]] bool GetButton(Button button) const;
  void ReleaseAll() { p1_state_ = 0; }
  [[nodiscard]] uint16_t GetP1State() const { return p1_state_; }

  // CpuMmio dispatches the joypad register offsets here. Manual reads return a
  // single bit in bit 0 with bits 7-1 left as open-bus (driven_mask=0x01).
  // Auto-joypad reads drive all 8 bits.
  [[nodiscard]] uint8_t ReadJoySer0();
  [[nodiscard]] uint8_t ReadJoySer1() const { return 0x00U; }  // no P2
  void WriteJoySer0(uint8_t data);                             // strobe write — only bit 0 matters

  [[nodiscard]] uint8_t ReadJoy1L() const { return static_cast<uint8_t>(p1_state_ & 0xFFU); }
  [[nodiscard]] uint8_t ReadJoy1H() const { return static_cast<uint8_t>((p1_state_ >> 8U) & 0xFFU); }

 private:
  uint16_t p1_state_ = 0;        // current button bitmask (auto-joypad layout)
  uint16_t shift_register_ = 0;  // latched snapshot used by manual serial reads
  uint8_t shift_count_ = 0;      // number of bits already clocked out (0..16)
  bool strobe_high_ = false;     // last bit-0 written to $4016
};

}  // namespace pupsnes
