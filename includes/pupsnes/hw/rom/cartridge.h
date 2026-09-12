#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <memory>

#include "pupsnes/core/device.h"
#include "pupsnes/hw/rom/cart_profile.h"
#include "pupsnes/hw/rom/rom_format.h"

namespace pupsnes {

class Mapper;
class SystemBus;

class Cartridge : public Device {
 public:
  static constexpr std::size_t kLoROMWindowSize = 32U * 1024U;
  // HiROM exposes a full 64 KiB CPU bank as ROM bytes; banks $00-$3F and
  // $80-$BF mirror the upper half ($8000-$FFFF) of the same 64 KiB stride.
  static constexpr std::size_t kHiROMBankSize = 64U * 1024U;

  // SRAM page offsets are tagged with this bit in the SystemBus page table's
  // `base_offset`, distinguishing them from ROM offsets so that the slow-path
  // ReadRegister / WriteRegister fallbacks and the debug API can route to the
  // SRAM buffer instead of indexing into rom_.
  static constexpr uint32_t kSramOffsetTag = 0x80000000U;

  explicit Cartridge(SNES& snes);
  ~Cartridge() override;

  [[nodiscard]] const char* DeviceName() const override { return "Cartridge"; }

  [[nodiscard]] MapperKind GetMapperKind() const { return mapper_kind_; }

  // Internal 21-byte ROM title from the header ($FFC0 HiROM / $7FC0 LoROM),
  // trimmed of trailing spaces. Returns empty when no ROM is loaded.
  [[nodiscard]] std::string GetInternalTitle() const;

  // Country/region byte from the header ($FFD9 HiROM / $7FD9 LoROM). Returns
  // 0xFF (open-bus shape) when no ROM is loaded.
  [[nodiscard]] uint8_t GetCountryCode() const;

  // True when the header map-mode byte advertises FastROM ($30 LoROM-fast,
  // $31 HiROM-fast, $35 ExHiROM-fast). Active FastROM is a runtime MEMSEL
  // state owned by CpuMmio; this is the cart-side capability.
  [[nodiscard]] bool IsFastRomCapable() const;

  // Validate and ingest the bytes as a LoROM image. On success returns
  // {ok=true, detected_kind=kLoROM} and the cartridge is ready for MapLoRom;
  // on failure returns ok=false with a specific message and leaves the
  // cartridge unchanged.
  RomLoadResult LoadLoRom(std::span<const uint8_t> rom_data);
  void MapLoRom(SystemBus& bus);

  // Same as LoadLoRom for HiROM images.
  RomLoadResult LoadHiRom(std::span<const uint8_t> rom_data);
  void MapHiRom(SystemBus& bus);

  // ExHiROM (Tales of Phantasia, Dai Kaiju Monogatari 2). Header at
  // $40FFB0; banks $00-$7D see the smaller half of ROM, banks $80-$FF see
  // the bigger 4 MiB half.
  RomLoadResult LoadExHiRom(std::span<const uint8_t> rom_data);
  void MapExHiRom(SystemBus& bus);

  // Re-map the active mapper's fast-bank range with the access speed selected
  // by `fast`. 6 master cycles when MEMSEL bit 0 is set, 8 otherwise. No-op
  // when no ROM is mapped (e.g. before Load*Rom + Map*Rom have run).
  //
  // LoROM: banks $80-$FF, pages $80-$FF.
  // HiROM: banks $80-$BF pages $80-$FF (half-bank ROM) plus banks $C0-$FF
  //        pages $00-$FF (full-bank ROM).
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

  // Mapper-facing accessors. The Mapper strategy object reads ROM bytes to
  // wire page-table fast_read_ptr windows, and reads/writes SRAM bytes
  // through the slow path so dirty tracking stays accurate. These spans are
  // safe to hold across MapInitial / OnMemSelChanged because Cartridge
  // never resizes its buffers after construction.
  [[nodiscard]] std::span<const uint8_t> RomView() const { return rom_; }
  [[nodiscard]] uint8_t* SramData() { return sram_.empty() ? nullptr : sram_.data(); }

 private:
  std::vector<uint8_t> rom_;
  std::vector<uint8_t> sram_;
  std::unique_ptr<Mapper> mapper_;
  bool sram_dirty_ = false;
  bool lorom_mapped_ = false;
  bool hirom_mapped_ = false;
  bool exhirom_mapped_ = false;
  MapperKind mapper_kind_ = MapperKind::kNone;
};

}  // namespace pupsnes
