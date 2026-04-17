#include "pupsnes/hw/snes.h"

#include <format>
#include <iostream>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/cartridge.h"
#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
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
      wram(std::make_unique<WRAM>(this)) {
  wram->MapSystemBus(*system_bus);
}

pupsnes::SNES::~SNES() = default;
pupsnes::Device* pupsnes::SNES::GetDevice(DeviceIdT id) const { return id < devices_.size() ? devices_[id] : nullptr; }

void pupsnes::SNES::LoadLoRom(std::span<const uint8_t> rom_data) {
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

pupsnes::DeviceIdT pupsnes::SNES::RegisterDevice(Device* device) {
  auto id = static_cast<DeviceIdT>(devices_.size());
  devices_.push_back(device);
  return id;
}

void pupsnes::SNES::DebugPrintInfo() {
  std::cerr << "SNES Info:\n";
  std::cerr << std::format("  Time (master): {}\n", time_now_);

  scheduler->DebugPrintNextEvent();
  scheduler->DebugPrintEventQueue();
}
