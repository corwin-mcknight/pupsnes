#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "pupsnes/hw/device.h"

namespace pupsnes {

class SystemBus;

enum class MapperKind : uint8_t {
  kNone = 0,
  kLoROM = 1,
  kHiROM = 2,
};

class Cartridge : public Device {
 public:
  static constexpr std::size_t kLoROMWindowSize = 32U * 1024U;

  // SRAM page offsets are tagged with this bit in the SystemBus page table's
  // `base_offset`, distinguishing them from ROM offsets so that the slow-path
  // ReadRegister / WriteRegister fallbacks and the debug API can route to the
  // SRAM buffer instead of indexing into rom_.
  static constexpr uint32_t kSramOffsetTag = 0x80000000U;

  explicit Cartridge(SNES* snes);
  ~Cartridge() override = default;

  [[nodiscard]] MapperKind GetMapperKind() const { return mapper_kind_; }

  void LoadLoRom(std::span<const uint8_t> rom_data);
  void MapLoRom(SystemBus& bus);

  // Re-map the LoROM fast-bank range ($80-$FD, pages $80-$FF) with the access
  // speed selected by the supplied flag. 6 master cycles when fast=true (MEMSEL
  // bit 0 set), 8 master cycles otherwise. No-op when LoROM is not currently
  // mapped (e.g. before LoadLoRom + MapLoRom have run, or for a HiROM image).
  void OnMemSelChanged(SystemBus& bus, bool fast);

  [[nodiscard]] MmioReadResult ReadRegister(uint32_t offset, TimeMasterT current_time) override;
  void WriteRegister(uint32_t offset, uint8_t data, TimeMasterT current_time) override;
  [[nodiscard]] std::optional<uint8_t> HandleDebugRead(uint32_t offset) const override;
  bool HandleDebugWrite(uint32_t offset, uint8_t data) override;

  [[nodiscard]] std::size_t Size() const { return rom_.size(); }

  // Size of the SRAM region declared by the ROM header. 0 means no SRAM is
  // present (e.g. action games with no save data) and the SRAM banks remain
  // unmapped / open bus.
  [[nodiscard]] std::size_t SramSize() const { return sram_.size(); }

  // Read-only view of the current SRAM contents. Used by the host to persist
  // saves to disk.
  [[nodiscard]] std::span<const uint8_t> SramView() const { return sram_; }

  // Replace SRAM contents with `data`. Bytes beyond `SramSize()` are ignored;
  // a short `data` leaves the tail zeroed. Clears the dirty flag — this is the
  // load path, not a write the game performed.
  void LoadSram(std::span<const uint8_t> data);

  // True when at least one byte of SRAM has been written since the last
  // ClearSramDirty(). The host uses this to decide whether to flush a save.
  [[nodiscard]] bool SramDirty() const { return sram_dirty_; }
  void ClearSramDirty() { sram_dirty_ = false; }

 private:
  std::vector<uint8_t> rom_;
  std::vector<uint8_t> sram_;
  bool sram_dirty_ = false;
  bool lorom_mapped_ = false;
  MapperKind mapper_kind_ = MapperKind::kNone;

  void MapLoRomSram(SystemBus& bus);
};

}  // namespace pupsnes
