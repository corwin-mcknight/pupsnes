#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "pupsnes/core/device.h"

namespace pupsnes {

// THROWAWAY — NOT A REAL APU.
//
// Blargg-style fake-APU used only to get ROMs past the SPC700 IPL handshake
// so we can see a title screen. No SPC700 execution, no S-DSP, no audio, no
// timing model. The real APU (scp700/, sdsp/) replaces this entirely — delete
// this file on the first day the SPC700 core boots.
//
// Two-state fake with a stall-detection escape hatch:
//
//   kSignature (initial, and re-entered on stall):
//     * Reads return $AA on port 0, $BB on port 1, $00 on ports 2/3 — the IPL
//       "ready" signature the CPU spins on after power-on and between upload
//       blocks.
//     * CPU writes to any port are dropped while the signature is pinned so
//       init code (memory clears, page-zero blasters) can't overwrite it.
//     * When the CPU writes $CC to port 0 — the value the real IPL's inner
//       wait loop (`CMP $F4,#$CC`) is looking for — port 0's read latch
//       becomes $CC (the kick-byte ack) and the fake transitions to kEcho.
//
//   kEcho (during an upload block):
//     * Every CPU write to port N updates port N's read latch directly, so the
//       per-byte upload handshake ("write counter → read counter back as ack")
//       completes on the first CMP.
//     * After the real IPL finishes a block, the uploaded SPC program is what
//       normally re-arms the $AA/$BB signature so the next block can start.
//       We have no SPC700, so we substitute a stall detector: if the CPU does
//       kStallReadThreshold consecutive port reads with no intervening port
//       write, it's almost certainly spinning on a signature-wait loop — we
//       flip back to kSignature and reset the read latches, unblocking the
//       loop on its next iteration.
//
// Known limitations (acceptable for "boot-only"):
//   * The stall threshold is a heuristic. Games that legitimately poll a port
//     without writing (rare — typical use is bidirectional) may false-flip.
//   * No audio, no DSP, no APU timer counters. Games that read APU-driven
//     counters for music/timing will desync.
//   * Games whose upload kick uses a value other than $CC on port 0 will stay
//     pinned at the signature forever.
class ApuStub : public Device {
 public:
  // Four CPU-visible I/O ports at $2140-$2143, mirrored through $217F on the
  // B-bus page. Dispatched to by the PPU which owns page $21 today.
  static constexpr uint32_t kPortBase = 0x2140U;
  static constexpr uint32_t kPortEnd = 0x2180U;  // exclusive
  static constexpr std::size_t kNumPorts = 4;

  static constexpr uint8_t kResetPort0 = 0xAAU;
  static constexpr uint8_t kResetPort1 = 0xBBU;
  // The IPL's inner wait loop (`CMP $F4,#$CC`) breaks when the CPU writes
  // $CC to port 0 — that same value is what we use to leave signature mode.
  static constexpr uint8_t kUploadKickByte = 0xCCU;
  // Master cycles after reset before the IPL signature ($AA/$BB) becomes
  // visible on ports 0/1. Real hardware needs the SPC700 IPL ROM to be ready
  // before the CPU's signature-wait loop can exit; a too-short delay lets the
  // loop satisfy on the first read. SMW does ~35k master cycles of setup
  // (memory clears) before reaching its `CMP $2140` loop, so the delay must
  // extend past that point to be observable. 65k master cycles ≈ 3 ms at
  // 21.477 MHz, comfortably past SMW's loop entry while still well under one
  // frame, and large enough that less-init-heavy games still loop a few
  // times.
  static constexpr TimeMasterT kSignatureDelayMaster = 65536;
  // Consecutive port reads without an intervening port write that we tolerate
  // before concluding the CPU is spinning on a signature-wait loop. A 16-bit
  // `CMP $2140` is 2 bus reads; at this threshold a stuck loop bounces back
  // to signature mode within ~4 iterations, while a legitimate per-byte echo
  // loop (read → write → read → write ...) never exceeds 1 read between
  // writes so it can't false-flip.
  static constexpr uint8_t kStallReadThreshold = 8;

  explicit ApuStub(SNES& snes);
  ~ApuStub() override = default;

  [[nodiscard]] const char* DeviceName() const override { return "APU (stub)"; }

  void Reset();

  [[nodiscard]] MmioReadResult ReadRegister(uint32_t offset, TimeMasterT current_time) override;
  void WriteRegister(uint32_t offset, uint8_t data, TimeMasterT current_time) override;
  [[nodiscard]] std::optional<uint8_t> HandleDebugRead(uint32_t offset) const override;
  bool HandleDebugWrite(uint32_t offset, uint8_t data) override;

  [[nodiscard]] uint8_t GetPort(std::size_t index) const { return ports_[index & 0x3U]; }
  [[nodiscard]] bool HasConsumedSignature() const { return signature_consumed_; }

 private:
  void ArmSignature();

  std::array<uint8_t, kNumPorts> ports_{};
  bool signature_consumed_ = false;
  uint8_t read_streak_ = 0;  // consecutive port reads since the last port write
};

}  // namespace pupsnes
