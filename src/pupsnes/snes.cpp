#include "pupsnes/hw/snes.h"

#include <format>
#include <iostream>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/5a22/cpu_mmio.h"
#include "pupsnes/hw/apu_stub.h"
#include "pupsnes/hw/cartridge.h"
#include "pupsnes/hw/device.h"
#include "pupsnes/hw/dma_controller.h"
#include "pupsnes/hw/joypad.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/hw/systembus.h"
#include "pupsnes/hw/wram.h"
#include "pupsnes/rom_format.h"

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
      dma(std::make_unique<DmaController>(this)),
      apu_stub(std::make_unique<ApuStub>(this)),
      joypad(std::make_unique<Joypad>(this)),
      ppu(std::make_unique<Ppu>(this)) {
  wram->MapSystemBus(*system_bus);
  cpu_mmio->MapSystemBus(*system_bus);
  dma->MapSystemBus(*system_bus);
  ppu->MapSystemBus(*system_bus);
}

pupsnes::SNES::~SNES() = default;
pupsnes::Device* pupsnes::SNES::GetDevice(DeviceIdT id) const { return id < devices_.size() ? devices_[id] : nullptr; }

pupsnes::RomLoadResult pupsnes::SNES::LoadLoRom(std::span<const uint8_t> rom_data) {
  // Validate before touching MMIO state. A rejected load must leave the
  // previous cartridge mapping intact — clearing MEMSEL up front would
  // visibly disturb a running game even though no new ROM ended up loaded.
  RomLoadResult result = cartridge->LoadLoRom(rom_data);
  if (!result.ok) {
    return result;
  }
  // Mirror power-cycle semantics: a fresh cartridge insert drops FASTROM back
  // to slow regardless of what the previous ROM left in $420D. Reset the MMIO
  // register so the cached memsel_ and the new page table agree from the
  // first cycle. Without this, the debugger's FASTROM indicator stays green
  // after Load across cartridge swaps.
  cpu_mmio->Reset();
  cartridge->MapLoRom(*system_bus);
  return result;
}

pupsnes::RomLoadResult pupsnes::SNES::LoadHiRom(std::span<const uint8_t> rom_data) {
  RomLoadResult result = cartridge->LoadHiRom(rom_data);
  if (!result.ok) {
    return result;
  }
  cpu_mmio->Reset();
  cartridge->MapHiRom(*system_bus);
  return result;
}

pupsnes::RomLoadResult pupsnes::SNES::LoadRom(std::span<const uint8_t> rom_data) {
  // Score both header candidates and dispatch to the winning loader. The
  // map mode byte is the primary signal; a valid checksum on one candidate
  // breaks ties when neither map mode byte is in range.
  if (rom_data.empty()) {
    return {false, MapperKind::kNone, "ROM is empty (0 bytes)"};
  }
  if (HasSmcCopierHeader(rom_data.size())) {
    return {false, MapperKind::kNone,
            std::format("ROM still has a {}-byte SMC copier header; strip it before loading.", kSmcCopierHeaderSize)};
  }
  const bool lorom_map = IsLoRomMapModeByte(LoRomMapModeByte(rom_data));
  const bool hirom_map = IsHiRomMapModeByte(HiRomMapModeByte(rom_data));
  const bool lorom_csum = LoRomChecksumValid(rom_data);
  const bool hirom_csum = HiRomChecksumValid(rom_data);

  if (hirom_map && !lorom_map) return LoadHiRom(rom_data);
  if (lorom_map && !hirom_map) return LoadLoRom(rom_data);
  if (lorom_map && hirom_map) {
    // Both map mode bytes legal — break with the checksum, then default to
    // LoROM which is the more common cartridge format.
    if (hirom_csum && !lorom_csum) return LoadHiRom(rom_data);
    return LoadLoRom(rom_data);
  }
  // Neither map mode byte recognised: fall back to checksum.
  if (hirom_csum && !lorom_csum) return LoadHiRom(rom_data);
  if (lorom_csum && !hirom_csum) return LoadLoRom(rom_data);
  // No discriminator at all. Default to LoROM — homebrew / test ROMs without
  // a real header almost always intend LoROM since that's what the standard
  // ld65 LoROM linker config emits.
  return LoadLoRom(rom_data);
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
  dma->Reset();
  apu_stub->Reset();
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

pupsnes::ApuStub& pupsnes::SNES::GetApuStub() { return *apu_stub; }
const pupsnes::ApuStub& pupsnes::SNES::GetApuStub() const { return *apu_stub; }

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
