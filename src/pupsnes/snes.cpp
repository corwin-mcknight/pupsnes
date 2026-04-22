#include "pupsnes/hw/snes.h"

#include <format>
#include <iostream>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/5a22/cpu_mmio.h"
#include "pupsnes/hw/cartridge.h"
#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/hw/systembus.h"
#include "pupsnes/hw/wram.h"

// --- Device ---

pupsnes::Device::Device(SNES* snes) : snes_(snes) {
  if (snes_ != nullptr) {
    device_id_ = snes_->RegisterDevice(this);
  }
}

// --- SNES ---

pupsnes::SNES::SNES()
    : cpu(std::make_unique<CPU>(this)),
      cartridge(std::make_unique<Cartridge>(this)),
      scheduler(std::make_unique<Scheduler>(this)),
      system_bus(std::make_unique<SystemBus>(this)),
      wram(std::make_unique<WRAM>(this)),
      cpu_mmio(std::make_unique<CpuMmio>(this)),
      ppu(std::make_unique<Ppu>(this)) {
  wram->MapSystemBus(*system_bus);
  cpu_mmio->MapSystemBus(*system_bus);
  ppu->MapSystemBus(*system_bus);
}

pupsnes::SNES::~SNES() = default;
pupsnes::Device* pupsnes::SNES::GetDevice(DeviceIdT id) const { return id < devices_.size() ? devices_[id] : nullptr; }

void pupsnes::SNES::LoadLoRom(std::span<const uint8_t> rom_data) {
  // Mirror power-cycle semantics: a fresh cartridge insert drops FASTROM back
  // to slow regardless of what the previous ROM left in $420D. Reset the MMIO
  // register *before* re-mapping so the cached memsel_ and the new page table
  // agree from the first cycle. Without this, the debugger's FASTROM indicator
  // stays green after Load across cartridge swaps.
  cpu_mmio->Reset();
  cartridge->LoadLoRom(rom_data);
  cartridge->MapLoRom(*system_bus);
}

void pupsnes::SNES::Reset() {
  time_now_ = 0;
  scheduler->Reset();
  for (Device* device : devices_) {
    if (device != nullptr) {
      device->SetLocalTime(0);
    }
  }
  cpu_mmio->Reset();
  // Reset the PPU before the CPU: the CPU's reset vector fetch may pass
  // through page $21 (cartridge DBs), and the PPU needs its shadow / decoded
  // fields cleared before any bus traffic arrives.
  ppu->Reset();
  cpu->Reset();
}

pupsnes::CPU& pupsnes::SNES::GetCpu() { return *cpu; }
const pupsnes::CPU& pupsnes::SNES::GetCpu() const { return *cpu; }

pupsnes::Cartridge& pupsnes::SNES::GetCartridge() { return *cartridge; }
const pupsnes::Cartridge& pupsnes::SNES::GetCartridge() const { return *cartridge; }

pupsnes::Scheduler& pupsnes::SNES::GetScheduler() { return *scheduler; }
const pupsnes::Scheduler& pupsnes::SNES::GetScheduler() const { return *scheduler; }

pupsnes::SystemBus& pupsnes::SNES::GetSystemBus() { return *system_bus; }
const pupsnes::SystemBus& pupsnes::SNES::GetSystemBus() const { return *system_bus; }

pupsnes::WRAM& pupsnes::SNES::GetWram() { return *wram; }
const pupsnes::WRAM& pupsnes::SNES::GetWram() const { return *wram; }

pupsnes::CpuMmio& pupsnes::SNES::GetCpuMmio() { return *cpu_mmio; }
const pupsnes::CpuMmio& pupsnes::SNES::GetCpuMmio() const { return *cpu_mmio; }

pupsnes::Ppu& pupsnes::SNES::GetPpu() { return *ppu; }
const pupsnes::Ppu& pupsnes::SNES::GetPpu() const { return *ppu; }

pupsnes::DeviceIdT pupsnes::SNES::RegisterDevice(Device* device) {
  auto id = static_cast<DeviceIdT>(devices_.size());
  devices_.push_back(device);
  return id;
}

void pupsnes::SNES::MachineSync(TimeMasterT target) {
  for (Device* device : devices_) {
    if (device != nullptr) {
      device->CatchUpTo(target);
    }
  }
}
