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

  explicit CpuMmio(SNES* snes);
  ~CpuMmio() override = default;

  void MapSystemBus(SystemBus& bus);

  // Clear all CPU MMIO register state. Called from SNES::Reset so a soft reset
  // (or loading a new ROM) drops FASTROM back to slow until the new ROM's init
  // code re-asserts MEMSEL. Also re-maps the LoROM fast banks to 8 mcyc so the
  // page table matches the cleared register.
  void Reset();

  [[nodiscard]] TickResult Tick(TimeMasterDeltaT budget) override;
  void OnEvent(const SchedulerEvent& event) override;
  [[nodiscard]] uint8_t ReadRegister(uint32_t offset) override;
  void WriteRegister(uint32_t offset, uint8_t data) override;
  [[nodiscard]] std::optional<uint8_t> HandleDebugRead(uint32_t offset) const override;
  bool HandleDebugWrite(uint32_t offset, uint8_t data) override;

  [[nodiscard]] bool IsFastRomEnabled() const { return (memsel_ & 0x01U) != 0U; }
  [[nodiscard]] uint8_t GetMemSel() const { return memsel_; }

 private:
  uint8_t memsel_ = 0;
};

}  // namespace pupsnes
