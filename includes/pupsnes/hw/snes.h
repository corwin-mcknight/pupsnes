#pragma once

#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "pupsnes/rom_format.h"
#include "pupsnes/types.h"

namespace pupsnes {
class ApuStub;
class CPU;
class Cartridge;
class CpuMmio;
class DmaController;
class Joypad;
class Ppu;
class Scheduler;
class SystemBus;
class Device;
class WRAM;
struct FrameBufferView;

class SNES {
 public:
  using FrameReadyCallback = std::function<void(const FrameBufferView&)>;

 private:
  TimeMasterT time_now_ = 0;
  std::vector<Device*> devices_;  // Non-owning. Devices register themselves; caller owns them.
  FrameReadyCallback frame_ready_callback_;

 public:
  std::unique_ptr<CPU> cpu;
  std::unique_ptr<Cartridge> cartridge;
  std::unique_ptr<Scheduler> scheduler;
  std::unique_ptr<SystemBus> system_bus;
  std::unique_ptr<WRAM> wram;
  std::unique_ptr<CpuMmio> cpu_mmio;
  std::unique_ptr<DmaController> dma;
  // Throwaway fake-APU — declared before the PPU so PPU can delegate page-$21
  // APU-port accesses to it. Replace when the real SPC700 core lands.
  std::unique_ptr<ApuStub> apu_stub;
  // P1 controller. Owns the live button state edited by the debug UI and the
  // manual-serial shift register CpuMmio dispatches $4016 reads to.
  std::unique_ptr<Joypad> joypad;
  // The PPU is declared last so it registers after every other Device and
  // gets the highest DeviceIdT. This keeps existing test expectations about
  // device-ID assignment for CPU / cartridge / WRAM / CpuMmio stable.
  std::unique_ptr<Ppu> ppu;

  SNES();
  ~SNES();

  // Load a cartridge image as the named mapper. On success the bus is fully
  // re-mapped and the result carries a one-line detection summary. On
  // failure the previous cartridge state is left untouched and the result's
  // `message` explains specifically what was wrong (empty, copier header
  // present, mapper mismatch, etc.).
  RomLoadResult LoadLoRom(std::span<const uint8_t> rom_data);
  RomLoadResult LoadHiRom(std::span<const uint8_t> rom_data);
  // Auto-detect the mapper from the header and load. Returns the same
  // result type; `detected_kind` names which path was taken when ok=true,
  // and on failure `message` describes why no mapper claimed the ROM.
  RomLoadResult LoadRom(std::span<const uint8_t> rom_data);
  void Reset();

  [[nodiscard]] TimeMasterT GetMasterTime() const { return time_now_; }
  void SetMasterTime(TimeMasterT t) { time_now_ = t; }
  // Advance every registered Device's internal state to `target`. Called by
  // RunControl's TickFrame after the CPU yields, before signal events fire.
  void MachineSync(TimeMasterT target);

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
  [[nodiscard]] DmaController& GetDma();
  [[nodiscard]] const DmaController& GetDma() const;
  [[nodiscard]] Joypad& GetJoypad();
  [[nodiscard]] const Joypad& GetJoypad() const;
  [[nodiscard]] Ppu& GetPpu();
  [[nodiscard]] const Ppu& GetPpu() const;
  [[nodiscard]] ApuStub& GetApuStub();
  [[nodiscard]] const ApuStub& GetApuStub() const;

  // Register a frontend-side callback invoked by the PPU at end-of-frame.
  // Copying the std::function here is intentional: callers typically set it
  // once at startup. Passing an empty callback clears the hook.
  void SetFrameReadyCallback(FrameReadyCallback callback) { frame_ready_callback_ = std::move(callback); }
  void FireFrameReady(const FrameBufferView& view) const {
    if (frame_ready_callback_) {
      frame_ready_callback_(view);
    }
  }

  DeviceIdT RegisterDevice(Device* device);
  [[nodiscard]] Device* GetDevice(DeviceIdT id) const;
};
}  // namespace pupsnes
