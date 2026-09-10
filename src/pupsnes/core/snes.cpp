#include "pupsnes/core/snes.h"

#include <format>
#include <iostream>
#include <string>
#include <utility>

#include "pupsnes/core/device.h"
#include "pupsnes/core/emu_event.h"
#include "pupsnes/core/scheduler.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/5a22/cpu_mmio.h"
#include "pupsnes/hw/5a22/dma_controller.h"
#include "pupsnes/hw/apu/apu.h"
#include "pupsnes/hw/input/joypad.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/hw/rom/rom_format.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/memory/systembus.h"
#include "pupsnes/memory/wram.h"

// --- Device ---

pupsnes::Device::Device(SNES& snes) : snes_(&snes) { device_id_ = snes_->RegisterDevice(this); }

pupsnes::Device::~Device() { snes_->DeregisterDevice(device_id_); }

// --- SNES ---

pupsnes::SNES::SNES()
    : cpu(std::make_unique<CPU>(*this)),
      cartridge(std::make_unique<Cartridge>(*this)),
      scheduler(std::make_unique<Scheduler>(*this)),
      system_bus(std::make_unique<SystemBus>(*this)),
      wram(std::make_unique<WRAM>(*this)),
      cpu_mmio(std::make_unique<CpuMmio>(*this)),
      dma(std::make_unique<DmaController>(*this)),
      apu(std::make_unique<Apu>(*this)),
      joypad(std::make_unique<Joypad>(*this)),
      ppu(std::make_unique<Ppu>(*this)) {
  registry_.RegisterBuiltins();
  wram->MapSystemBus(*system_bus);
  cpu_mmio->MapSystemBus(*system_bus);
  dma->MapSystemBus(*system_bus);
  ppu->MapSystemBus(*system_bus);
}

pupsnes::SNES::~SNES() {
  // Set the flag BEFORE member dtors fire. Member-reverse-destruction destroys
  // ppu first, then joypad, ..., then system_bus, then scheduler, then
  // cartridge, then cpu. Each Device's dtor calls DeregisterDevice via the
  // base class — checking destroying_ short-circuits those calls so we don't
  // try to scrub page-table state on a SystemBus that's already being torn
  // down (it sits between cartridge and cpu in destruction order).
  destroying_ = true;
}

pupsnes::Device* pupsnes::SNES::GetDevice(DeviceIdT id) const { return id < devices_.size() ? devices_[id] : nullptr; }

void pupsnes::SNES::DeregisterDevice(DeviceIdT id) {
  if (destroying_) {
    return;
  }
  if (id < devices_.size()) {
    devices_[id] = nullptr;
  }
  if (system_bus) {
    system_bus->UnmapByDeviceId(id);
  }
}

pupsnes::BuildResult pupsnes::SNES::LoadRomWithProfile(const CartProfile& profile, std::span<const uint8_t> rom_data) {
  // The builder constructs a fresh Cartridge (registered with this SNES,
  // gets a new DeviceId monotonically) and wires its pages into the
  // SystemBus before we touch the old cartridge. On success we install the
  // new cart by replacing the unique_ptr — the old cart's destructor then
  // calls DeregisterDevice → UnmapByDeviceId, scrubbing any stale page
  // entries that the new mapper didn't overwrite. On failure the old cart
  // and its page mappings are left untouched.
  BuildResult result = registry_.Build(this, rom_data, profile);
  if (result.ok && result.cart) {
    cartridge = std::move(result.cart);
    cpu_mmio->Reset();  // power-cycle FASTROM (matches legacy LoadLoRom).
    events::Emit(emu_event_sink_, time_now_, EmuEventKind::kRomLoaded, rom_data.size(),
                 static_cast<uint32_t>(profile.mapper));
  }
  return result;
}

pupsnes::BuildResult pupsnes::SNES::LoadRom(std::span<const uint8_t> rom_data) {
  // Run the structured detector and dispatch through the registry. On
  // detection failure (mapper == kNone) return a BuildResult that carries
  // the detection diagnostic so the caller can surface a specific reason.
  const auto detection = DetectCartProfile(rom_data);
  if (detection.profile.mapper == MapperKind::kNone) {
    BuildResult result;
    result.ok = false;
    result.profile = detection.profile;
    result.message = detection.diagnostic.empty() ? std::string{"Unrecognised cart shape"} : detection.diagnostic;
    return result;
  }
  return LoadRomWithProfile(detection.profile, rom_data);
}

void pupsnes::SNES::Reset() {
  // Construct the requested DSP backend on reset; pending UI changes do not
  // replace an active backend or discard its register state mid-run.
  sdsp_mode_live_ = sdsp_mode_pending_;
  time_now_ = 0;
  scheduler->Reset();
  for (Device* device : devices_) {
    if (device != nullptr) {
      device->SetLocalTime(0);
    }
  }
  cpu_mmio->Reset();
  dma->Reset();
  apu->Reset(sdsp_mode_live_);
  joypad->Reset();
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

pupsnes::DmaController& pupsnes::SNES::GetDma() { return *dma; }
const pupsnes::DmaController& pupsnes::SNES::GetDma() const { return *dma; }

pupsnes::Joypad& pupsnes::SNES::GetJoypad() { return *joypad; }
const pupsnes::Joypad& pupsnes::SNES::GetJoypad() const { return *joypad; }

pupsnes::Ppu& pupsnes::SNES::GetPpu() { return *ppu; }
const pupsnes::Ppu& pupsnes::SNES::GetPpu() const { return *ppu; }

pupsnes::Apu& pupsnes::SNES::GetApu() { return *apu; }
const pupsnes::Apu& pupsnes::SNES::GetApu() const { return *apu; }

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
