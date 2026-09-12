#pragma once

#include <cstdint>
#include <optional>

#include "pupsnes/core/device.h"

namespace pupsnes {

class SystemBus;

// 5A22 CPU registers and controller ports on banks $00-$3F / $80-$BF.
// Owns MEMSEL, NMI/IRQ controls, programmable I/O, and DMA enable registers;
// delegates controller data to Joypad and transfers to DmaController, which
// also owns the $43xx channel registers. Unimplemented register reads leave
// the bus undriven and their writes are ignored.
class CpuMmio : public Device {
 public:
  // MEMSEL ($420D): bit 0 selects the access speed for banks $80-$FD pages
  // $80-$FF. 0 = slow (8 master cycles), 1 = fast (6 master cycles).
  static constexpr uint32_t kMemSelOffset = 0x420DU;

  // NMITIMEN ($4200) — interrupt / joypad enable (write-only).
  // bit 7: VBlank NMI enable (gates the PPU /NMI line into the CPU's NMI
  //        flip-flop. Transition quirks are handled by
  //        CPU::OnNmiTimenChanged — see CpuMmio::WriteRegister).
  // bits 5:4: H/V-IRQ trigger mode — 00 disabled, 01 H-only, 10 V-only,
  //           11 both. Mode 00 clears the TIMEUP latch and de-asserts /IRQ.
  // bit 0: auto-joypad read enable (deferred).
  static constexpr uint32_t kNmiTimenOffset = 0x4200U;
  static constexpr uint8_t kNmiTimenNmiEnableMask = 0x80U;
  static constexpr uint8_t kNmiTimenVIrqEnableMask = 0x20U;
  static constexpr uint8_t kNmiTimenHIrqEnableMask = 0x10U;
  static constexpr uint8_t kNmiTimenIrqModeMask =
      static_cast<uint8_t>(kNmiTimenVIrqEnableMask | kNmiTimenHIrqEnableMask);
  static constexpr uint8_t kNmiTimenAutoJoypadMask = 0x01U;

  // WRIO ($4201) / RDIO ($4213) — programmable joypad-port I/O. With no
  // external device pulling a line low, RDIO reflects the WRIO open-collector
  // setting (0=driven low, 1=High-Z pulled high). WRIO bit 7 also gates the
  // PPU H/V latch; its 1->0 edge triggers the same capture as SLHV.
  static constexpr uint32_t kWrioOffset = 0x4201U;
  static constexpr uint32_t kRdioOffset = 0x4213U;
  static constexpr uint8_t kWrioHvLatchMask = 0x80U;

  // HTIMEL/H ($4207-$4208), VTIMEL/H ($4209-$420A) — 9-bit H/V-IRQ targets.
  // Write-only on hardware; the shadow is exposed for the debugger and tests.
  static constexpr uint32_t kHTimeLOffset = 0x4207U;
  static constexpr uint32_t kHTimeHOffset = 0x4208U;
  static constexpr uint32_t kVTimeLOffset = 0x4209U;
  static constexpr uint32_t kVTimeHOffset = 0x420AU;
  static constexpr uint16_t kHvTimeFieldMask = 0x01FFU;  // 9-bit

  // TIMEUP ($4211) — bit 7 is the H/V-IRQ latch. Reading clears the latch and
  // de-asserts /IRQ. Bits 6:0 are open-bus.
  static constexpr uint32_t kTimeUpOffset = 0x4211U;
  static constexpr uint8_t kTimeUpFlagMask = 0x80U;

  // RDNMI ($4210): bit 7 = VBlank-NMI latch (set at VBlank entry, cleared on
  // read), bits 6:4 = open-bus, bits 3:0 = 5A22 CPU revision. Fullsnes notes
  // real hardware reports revision 2 on all retail consoles.
  static constexpr uint32_t kRdNmiOffset = 0x4210U;
  static constexpr uint8_t kRdNmiVblankFlagMask = 0x80U;
  static constexpr uint8_t kRdNmiVersionMask = 0x0FU;
  static constexpr uint8_t kRdNmiCpuVersion = 0x02U;
  static constexpr uint8_t kRdNmiDrivenMask = kRdNmiVblankFlagMask | kRdNmiVersionMask;

  // HVBJOY ($4212): bit 7 = V-Blank flag, bit 6 = H-Blank flag, bit 0 = auto-
  // joypad busy. Bit-0 stays 0 until auto-joypad read is modeled. Computed
  // on demand from PPU dot/scanline state via Ppu::QueryHvbStatus so polling
  // loops see state current to the read's master cycle.
  static constexpr uint32_t kHvbJoyOffset = 0x4212U;
  static constexpr uint8_t kHvbJoyVblankMask = 0x80U;
  static constexpr uint8_t kHvbJoyHblankMask = 0x40U;
  static constexpr uint8_t kHvbJoyAutoJoypadMask = 0x01U;
  static constexpr uint8_t kHvbJoyDrivenMask = kHvbJoyVblankMask | kHvbJoyHblankMask | kHvbJoyAutoJoypadMask;

  // JOYSER0/JOYSER1 ($4016/$4017) — serial joypad ports. Joypad provides P1
  // serial data and the JOY1 result bytes; absent P2-P4 controllers read zero.
  // The timed auto-read sequence and HVBJOY busy interval remain unmodeled.
  static constexpr uint32_t kJoySer0Offset = 0x4016U;
  static constexpr uint32_t kJoySer1Offset = 0x4017U;
  static constexpr uint32_t kAutoJoyResultFirst = 0x4218U;
  static constexpr uint32_t kAutoJoyResultLast = 0x421FU;

  // MDMAEN ($420B): write-only, bit N = trigger general DMA on channel N.
  // HDMAEN ($420C): write-only, bit N = enable HDMA on channel N for the frame.
  // DmaController samples HDMAEN at frame init and gates channels with its
  // live value on each line.
  static constexpr uint16_t kMdmaEnOffset = 0x420BU;
  static constexpr uint16_t kHdmaEnOffset = 0x420CU;

  explicit CpuMmio(SNES& snes);
  ~CpuMmio() override = default;

  [[nodiscard]] const char* DeviceName() const override { return "CPU MMIO"; }

  void MapSystemBus(SystemBus& bus);

  // Clear all CPU MMIO register state. Called from SNES::Reset so a soft reset
  // (or loading a new ROM) drops FASTROM back to slow until the new ROM's init
  // code re-asserts MEMSEL. Also re-maps the LoROM fast banks to 8 mcyc so the
  // page table matches the cleared register.
  void Reset();

  [[nodiscard]] MmioReadResult ReadRegister(uint32_t offset, TimeMasterT current_time) override;
  void WriteRegister(uint32_t offset, uint8_t data, TimeMasterT current_time) override;
  [[nodiscard]] std::optional<uint8_t> HandleDebugRead(uint32_t offset) const override;
  bool HandleDebugWrite(uint32_t offset, uint8_t data) override;

  [[nodiscard]] bool IsFastRomEnabled() const { return (memsel_ & 0x01U) != 0U; }
  [[nodiscard]] uint8_t GetMemSel() const { return memsel_; }

  [[nodiscard]] uint8_t GetNmiTimen() const { return nmitimen_; }
  [[nodiscard]] bool GetNmiEnable() const { return (nmitimen_ & kNmiTimenNmiEnableMask) != 0U; }
  [[nodiscard]] bool GetVIrqEnable() const { return (nmitimen_ & kNmiTimenVIrqEnableMask) != 0U; }
  [[nodiscard]] bool GetHIrqEnable() const { return (nmitimen_ & kNmiTimenHIrqEnableMask) != 0U; }
  [[nodiscard]] bool GetAutoJoypadEnable() const { return (nmitimen_ & kNmiTimenAutoJoypadMask) != 0U; }
  [[nodiscard]] uint8_t GetWrio() const { return wrio_; }
  [[nodiscard]] bool IsHvLatchEnabled() const { return (wrio_ & kWrioHvLatchMask) != 0U; }

  // 9-bit H/V-IRQ targets (assembled from the two-byte writes to $4207/$4208
  // and $4209/$420A).
  [[nodiscard]] uint16_t GetHTime() const { return htime_; }
  [[nodiscard]] uint16_t GetVTime() const { return vtime_; }

  // TIMEUP ($4211) bit 7 — true while an H/V-IRQ is pending and unacknowledged.
  // This is the level the CPU's /IRQ pin sees: the CPU samples it at each
  // instruction boundary. Reading $4211 clears the latch; the level is also
  // cleared when NMITIMEN bits 5:4 transition to 00.
  [[nodiscard]] bool SampleIrqLine() const { return timeup_latch_; }

  // Scheduler-fence handler. Called from the kHIrqMatch signal at the master
  // cycle a programmed H/V match lands on. Sets the TIMEUP latch and asserts
  // /IRQ, then re-arms the next match according to the current trigger mode.
  // Lives on CpuMmio because it owns the register surface and IRQ line state.
  void HandleIrqMatch(TimeMasterT t);

  // Live $420C shadow, consumed by DmaController and exposed to the debugger.
  [[nodiscard]] uint8_t GetHdmaEn() const { return hdmaen_; }

 private:
  // (Re)compute when the next H/V-IRQ match cycle lands given the current
  // (nmitimen, htime, vtime) and master time, and schedule a kHIrqMatch
  // signal for it. Called whenever any of those inputs changes, and from
  // HandleIrqMatch after acknowledging a previous match. A no-op when the
  // IRQ-mode bits are 00 — without those bits set, there is no match.
  void RescheduleIrqMatchFrom(TimeMasterT now);

  uint8_t memsel_ = 0;
  uint8_t nmitimen_ = 0;
  uint8_t wrio_ = 0xFFU;
  uint8_t hdmaen_ = 0;

  // H/V-IRQ target registers (9-bit each). Built from two-byte writes — the
  // low byte writes to $4207/$4209 immediately and the high byte to $4208/
  // $420A overwrites the upper 8 bits (we only honour the 9-bit field).
  uint16_t htime_ = 0;
  uint16_t vtime_ = 0;
  // TIMEUP latch — set when the comparator matches, cleared on $4211 read or
  // when NMITIMEN bits 5:4 → 00.
  bool timeup_latch_ = false;
  // Master cycle of the currently scheduled kHIrqMatch event, or kNoMatch when
  // nothing is scheduled. Used to de-duplicate re-schedule requests when
  // multiple register writes hit before the event fires.
  static constexpr TimeMasterT kNoMatch = static_cast<TimeMasterT>(-1);
  TimeMasterT scheduled_match_time_ = kNoMatch;
};

}  // namespace pupsnes
