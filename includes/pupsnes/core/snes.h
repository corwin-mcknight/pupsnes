#pragma once

#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "pupsnes/core/types.h"
#include "pupsnes/hw/apu/sdsp.h"
#include "pupsnes/hw/rom/cart_registry.h"
#include "pupsnes/hw/rom/rom_format.h"

namespace pupsnes {
class ApuStub;
class CPU;
class Cartridge;
class CpuMmio;
class DmaController;
class EmuEventSink;
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
  // Auto-detect the cart profile from the ROM header and dispatch through
  // the registry. The returned BuildResult carries the new Cartridge (when
  // ok=true) which has already been installed on this SNES; the unique_ptr
  // remains live in BuildResult only so the caller can inspect the cart
  // alongside the message and profile fields.
  BuildResult LoadRom(std::span<const uint8_t> rom_data);

  // Same as LoadRom but skips detection and uses the caller-supplied profile.
  // Used for homebrew with broken headers ("force LoROM despite the bogus
  // map mode byte"), debug tooling, and tests that want to exercise a
  // specific (mapper, coproc) builder.
  BuildResult LoadRomWithProfile(const CartProfile& profile, std::span<const uint8_t> rom_data);

  // Access to the registry for tests and host code that want to register a
  // custom builder (e.g. a homebrew mapper not present in fullsnes).
  [[nodiscard]] CartridgeRegistry& Registry() { return registry_; }
  [[nodiscard]] const CartridgeRegistry& Registry() const { return registry_; }
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

  // Optional structured-event recorder (see pupsnes/core/emu_event.h).
  // Devices emit through this pointer via events::Emit, which tolerates
  // null (no recorder attached). Non-owning; the sink must outlive emission.
  void SetEmuEventSink(EmuEventSink* sink) { emu_event_sink_ = sink; }
  [[nodiscard]] EmuEventSink* GetEmuEventSink() const { return emu_event_sink_; }

  DeviceIdT RegisterDevice(Device* device);
  // Null the slot at `id` and clear any page-table entries that point at
  // this DeviceId. Monotonic: slots are never reused, so future Devices get
  // fresh IDs even after deregistration. No-op when destroying_ is true
  // (SNES destruction is destroying member Devices in cascade; the
  // SystemBus is already gone or about to be, and we don't want to scrub
  // page-table state that will die anyway).
  void DeregisterDevice(DeviceIdT id);
  [[nodiscard]] Device* GetDevice(DeviceIdT id) const;
  [[nodiscard]] std::size_t GetDeviceCount() const { return devices_.size(); }
  [[nodiscard]] bool IsDestroying() const { return destroying_; }

  // S-DSP backend selection. The "pending" value is what the debugger UI /
  // config has chosen; the "live" value is what the active APU is running.
  // Reset() copies pending into live so a startup-only switch actually takes
  // effect on the next reset cycle. Both default to kSimple (the cheaper
  // backend lands first and is the right dev-time default).
  [[nodiscard]] SdspMode GetSdspModePending() const { return sdsp_mode_pending_; }
  [[nodiscard]] SdspMode GetSdspModeLive() const { return sdsp_mode_live_; }
  void SetSdspModePending(SdspMode mode) { sdsp_mode_pending_ = mode; }

 private:
  CartridgeRegistry registry_;
  EmuEventSink* emu_event_sink_ = nullptr;
  SdspMode sdsp_mode_pending_ = SdspMode::kSimple;
  SdspMode sdsp_mode_live_ = SdspMode::kSimple;
  bool destroying_ = false;
};
}  // namespace pupsnes
