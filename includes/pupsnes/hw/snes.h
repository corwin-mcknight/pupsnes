#pragma once

#include <memory>
#include <span>
#include <vector>

#include "pupsnes/types.h"

namespace pupsnes {
class CPU;
class Cartridge;
class CpuMmio;
class Scheduler;
class SystemBus;
class Device;
class WRAM;

class SNES {
 private:
  TimeMasterT time_now_ = 0;
  std::vector<Device*> devices_;  // Non-owning. Devices register themselves; caller owns them.

 public:
  std::unique_ptr<CPU> cpu;
  std::unique_ptr<Cartridge> cartridge;
  std::unique_ptr<Scheduler> scheduler;
  std::unique_ptr<SystemBus> system_bus;
  std::unique_ptr<WRAM> wram;
  std::unique_ptr<CpuMmio> cpu_mmio;

  SNES();
  ~SNES();

  void LoadLoRom(std::span<const uint8_t> rom_data);
  void Reset();

  [[nodiscard]] TimeMasterT GetMasterTime() const { return time_now_; }
  void SetMasterTime(TimeMasterT t) { time_now_ = t; }
  [[nodiscard]] CPU& GetCpu();
  [[nodiscard]] const CPU& GetCpu() const;
  [[nodiscard]] Cartridge& GetCartridge();
  [[nodiscard]] const Cartridge& GetCartridge() const;
  [[nodiscard]] Scheduler& GetScheduler();
  [[nodiscard]] const Scheduler& GetScheduler() const;
  [[nodiscard]] SystemBus& GetSystemBus();
  [[nodiscard]] const SystemBus& GetSystemBus() const;
  [[nodiscard]] WRAM& GetWram();
  [[nodiscard]] const WRAM& GetWram() const;
  [[nodiscard]] CpuMmio& GetCpuMmio();
  [[nodiscard]] const CpuMmio& GetCpuMmio() const;

  DeviceIdT RegisterDevice(Device* device);
  [[nodiscard]] Device* GetDevice(DeviceIdT id) const;
};
}  // namespace pupsnes
