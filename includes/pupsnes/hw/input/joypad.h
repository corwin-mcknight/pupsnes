#pragma once

#include <cstdint>

#include "pupsnes/core/device.h"

namespace pupsnes {

// Two SNES standard controllers, one on each port.
//
// Holds the 16-bit auto-joypad words for P1 and P2 and implements both access
// paths a game might use:
//
//   * Auto-joypad read ($4218-$421B for JOY1 and JOY2). VBlank schedules a
//     4224-master-cycle serial poll, gated by $4200.0. Results accumulate as
//     bits arrive and remain latched until the next enabled poll.
//   * Manual serial read ($4016 strobe + shift). Standard sequence: write 1
//     then 0 to $4016 to latch both pads, then read $4016 or $4017 sixteen
//     times to clock out that port's bits MSB-first (B first). Reads past the
//     16-bit window return 1, matching the open data line on real hardware.
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

  explicit Joypad(SNES& snes);
  ~Joypad() override = default;

  [[nodiscard]] const char* DeviceName() const override { return "Joypad"; }

  // Reset clears the shift register, strobe latch, and shift counter so a
  // soft reset doesn't leave a partially-clocked manual read in flight. The
  // user-driven button state survives — the UI owns it and the user is the
  // source of truth, not the machine.
  void Reset();
  void CatchUpTo(TimeMasterT target) override;

  // Called by the PPU's VBlank scheduler fence, including frames with auto
  // polling disabled, to retain the free-running 256-cycle start phase.
  void OnVblankStart(TimeMasterT boundary);
  [[nodiscard]] bool AutoReadBusy(TimeMasterT current_time);

  // Port is 0 for P1 or 1 for P2. Defaults preserve existing P1 callers.
  void SetButton(Button button, bool pressed, unsigned port = 0);
  [[nodiscard]] bool GetButton(Button button, unsigned port = 0) const;
  void ReleaseAll(unsigned port);
  void ReleaseAll() {
    ReleaseAll(0);
    ReleaseAll(1);
  }
  [[nodiscard]] uint16_t GetP1State() const { return states_[0]; }
  [[nodiscard]] uint16_t GetP2State() const { return states_[1]; }

  // CpuMmio dispatches the joypad register offsets here. Manual reads return a
  // single bit in bit 0 with bits 7-1 left as open-bus (driven_mask=0x01).
  // Auto-joypad reads drive all 8 bits.
  [[nodiscard]] uint8_t ReadJoySer0();
  [[nodiscard]] uint8_t ReadJoySer1();
  void WriteJoySer0(uint8_t data);  // strobe write — only bit 0 matters

  [[nodiscard]] uint8_t ReadJoy1L() const { return static_cast<uint8_t>(auto_results_[0] & 0xFFU); }
  [[nodiscard]] uint8_t ReadJoy1H() const { return static_cast<uint8_t>(auto_results_[0] >> 8U); }
  [[nodiscard]] uint8_t ReadJoy2L() const { return static_cast<uint8_t>(auto_results_[1] & 0xFFU); }
  [[nodiscard]] uint8_t ReadJoy2H() const { return static_cast<uint8_t>(auto_results_[1] >> 8U); }

 private:
  uint16_t states_[2]{};  // current button bitmasks (auto-joypad layout)
  uint16_t auto_results_[2]{};
  uint16_t shift_registers_[2]{};
  uint8_t shift_counts_[2]{};
  bool strobe_high_ = false;  // last bit-0 written to $4016
  bool auto_sequence_active_ = false;
  bool auto_start_seen_ = false;
  TimeMasterT last_auto_start_ = 0;
  TimeMasterT next_auto_edge_ = 0;
  uint8_t auto_edge_ = 0;
  uint8_t sampled_bits_[2]{};
  void StartAutoRead(TimeMasterT start);
  [[nodiscard]] uint8_t ReadSerial(unsigned port);
};

}  // namespace pupsnes
