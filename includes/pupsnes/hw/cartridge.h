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

  [[nodiscard]] TickResult Tick(TimeMasterDeltaT budget) override;
  void OnEvent(const SchedulerEvent& event) override;
  [[nodiscard]] uint8_t ReadRegister(uint32_t offset) override;
  [[nodiscard]] std::optional<uint8_t> HandleDebugRead(uint32_t offset) const override;

  [[nodiscard]] std::size_t Size() const { return rom_.size(); }

 private:
  std::vector<uint8_t> rom_;
  bool lorom_mapped_ = false;
  MapperKind mapper_kind_ = MapperKind::kNone;
};

}  // namespace pupsnes
