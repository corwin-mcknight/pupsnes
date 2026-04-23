#pragma once

#include <cstdint>
#include <optional>

#include "pupsnes/hw/device.h"

namespace pupsnes {

class SystemBus;

// 5A22 CPU MMIO registers at $4200-$43FF on banks $00-$3F / $80-$BF.
//
// Today this device only stores MEMSEL ($420D) so the LoROM mapper can honor
// FASTROM. Other registers (NMITIMEN, joypad, HDMA) are stubbed — reads return
// open-bus sentinel and writes are accepted silently so ROMs can poke them
// without the bus rejecting the transaction.
class CpuMmio : public Device {
 public:
  // MEMSEL ($420D): bit 0 selects the access speed for banks $80-$FD pages
  // $80-$FF. 0 = slow (8 master cycles), 1 = fast (6 master cycles).
  static constexpr uint32_t kMemSelOffset = 0x420DU;

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
  static constexpr uint8_t kHvbJoyDrivenMask =
      kHvbJoyVblankMask | kHvbJoyHblankMask | kHvbJoyAutoJoypadMask;

  explicit CpuMmio(SNES* snes);
  ~CpuMmio() override = default;

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

 private:
  uint8_t memsel_ = 0;
};

}  // namespace pupsnes
