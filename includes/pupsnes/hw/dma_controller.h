#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "pupsnes/hw/device.h"

namespace pupsnes {

class SystemBus;

class DmaController : public Device {
 public:
  // Per-channel state mirrors fullsnes $43x0..$43x7. All eight bytes are R/W
  // shadow with no decode at write time — the trigger reads the live values.
  // The trailing bool fields are transient HDMA runtime state, not CPU-visible.
  struct ChannelState {
    uint8_t dmap = 0;               // $43x0
    uint8_t bbad = 0;               // $43x1
    uint16_t a1t = 0;               // $43x2/3 (low+high)
    uint8_t a1b = 0;                // $43x4
    uint16_t das = 0;               // $43x5/6 byte counter (GP-DMA, 0=64K) / indirect data ptr (HDMA)
    uint8_t dasb = 0;               // $43x7 (HDMA indirect data bank)
    uint8_t a2a = 0;                // $43x8 (HDMA source-table cursor low)
    uint8_t a2a_high = 0;           // $43x9 (HDMA source-table cursor high)
    uint8_t ntrl = 0;               // $43xA (HDMA line counter: bit 7 = repeat, bits 6:0 = lines left)
    bool hdma_do_transfer = false;  // HDMA runtime: transfer on the next per-line fire
    bool hdma_finished = false;     // HDMA runtime: count byte = 0 hit; channel done this frame
  };

  // Snapshot of one $420B trigger captured for the debugger UI. Pre-state is
  // recorded before Trigger()'s loop mutates the channel registers, so the
  // user can see what each channel was about to do even after das/a1t have
  // been consumed by the transfer.
  struct TriggerRecord {
    TimeMasterT start_time = 0;  // master_time when $420B was written
    TimeMasterT end_time = 0;    // master_time after all channels completed
    uint8_t channels_mask = 0;   // value written to $420B

    struct PerChannel {
      uint8_t dmap = 0;                // pre-trigger snapshot of $43x0
      uint8_t bbad = 0;                // pre-trigger snapshot of $43x1
      uint8_t a1b = 0;                 // pre-trigger snapshot of $43x4
      uint16_t a1t_start = 0;          // pre-trigger snapshot of $43x2/3
      uint16_t das_start = 0;          // pre-trigger snapshot of $43x5/6 (0=64K)
      uint32_t bytes_transferred = 0;  // das_start ? das_start : 65536
    };
    // Slots for channels not selected by channels_mask are default-initialized.
    std::array<PerChannel, 8> per_channel{};
  };

  static constexpr std::size_t kTriggerRingCapacity = 32;

  explicit DmaController(SNES* snes);
  ~DmaController() override = default;

  void MapSystemBus(SystemBus& bus);
  void Reset();

  [[nodiscard]] MmioReadResult ReadRegister(uint32_t offset, TimeMasterT current_time) override;
  void WriteRegister(uint32_t offset, uint8_t data, TimeMasterT current_time) override;
  [[nodiscard]] std::optional<uint8_t> HandleDebugRead(uint32_t offset) const override;
  bool HandleDebugWrite(uint32_t offset, uint8_t data) override;

  // Runs general DMA for the channels selected by `channels_mask` (bit N =
  // channel N). Returns the master time after all transfers complete.
  // Performs the bus reads/writes synchronously via SystemBus.
  TimeMasterT Trigger(uint8_t channels_mask, TimeMasterT start_time);

  [[nodiscard]] const ChannelState& GetChannelState(uint8_t channel) const { return channels_[channel & 7U]; }

  // Recent $420B triggers in oldest-first order. Bounded by
  // kTriggerRingCapacity; once full, oldest entries are overwritten.
  [[nodiscard]] std::size_t GetRecentTriggerCount() const;
  [[nodiscard]] const TriggerRecord& GetRecentTrigger(std::size_t index) const;
  [[nodiscard]] uint8_t GetLastTriggerMask() const { return last_trigger_mask_; }

  // HDMA accessors for tests and the debugger panel. `hdma_active_mask_` is
  // the snapshot of $420C HDMAEN taken at the most recent HDMA init; the live
  // CPU shadow is queried per-line.
  [[nodiscard]] uint8_t GetHdmaActiveMask() const { return hdma_active_mask_; }
  [[nodiscard]] TimeMasterT GetHdmaFrameBaseTime() const { return hdma_frame_base_time_; }

 private:
  // HDMA timing constants. Per fullsnes: HDMA init fires at V=0 H=6; per-line
  // transfer fires at V=0..V_END H=274. Each dot before H=323 is 4 master
  // cycles, so init = 6*4 = 24 mcyc into the frame, per-line line 0 = 274*4 =
  // 1096 mcyc. We hardcode V_END=224 (no overscan) for v1.
  static constexpr uint32_t kHdmaInitMasterCycles = 6U * 4U;       // V=0 H=6
  static constexpr uint32_t kHdmaPerLineMasterCycles = 274U * 4U;  // V=N H=274
  static constexpr uint32_t kHdmaLastVisibleV = 224U;              // V=224 last per-line fire
  static constexpr uint32_t kHdmaNormalLineCycles = 1364U;
  static constexpr uint32_t kHdmaFrameCycles = 262U * kHdmaNormalLineCycles;

  enum class HdmaPhase : uint8_t {
    kInit,
    kRunLine,
  };

  // Returns the live shadow byte for a valid DMA register offset
  // ($4300-$437F, local 0x0..0xA). Returns std::nullopt for the unused
  // $xB-$xF tail and for offsets outside the channel window so callers can
  // distinguish "not handled" from a real zero byte.
  [[nodiscard]] std::optional<uint8_t> ReadRegisterShadow(uint32_t offset) const;

  // Scheduler signal handler — dispatches between init and per-line phases
  // based on hdma_phase_, advances master_time + CPU local_time by the cycle
  // cost, and schedules the next HDMA signal.
  void OnHdmaSignal(TimeMasterT master_time);

  // Schedules the first HDMA signal of a frame (the init signal at V=0 H=6).
  // Called from Reset() and from per-line handler after V=kHdmaLastVisibleV.
  void ScheduleNextHdmaInit(TimeMasterT frame_base);

  // Runs the HDMA per-line work for one channel at master_time `t`. Returns
  // the master_time after the channel's bus accesses finish (caller uses the
  // delta to charge CPU stall cycles). Walks the per-line algorithm: optional
  // header reload, optional one-unit transfer, NTRL decrement, do_transfer
  // recompute.
  TimeMasterT HdmaRunChannelLine(uint8_t ch, TimeMasterT t);

  std::array<ChannelState, 8> channels_{};
  std::array<TriggerRecord, kTriggerRingCapacity> trigger_ring_{};
  std::size_t trigger_write_count_ = 0;
  uint8_t last_trigger_mask_ = 0;

  // HDMA scheduler state. `hdma_phase_` selects between init and per-line
  // dispatch on the next OnHdmaSignal fire. `hdma_next_v_` is the scanline
  // the next per-line fire will service (only meaningful when phase == kRunLine).
  // `hdma_active_mask_` is the per-frame snapshot of HDMAEN. `hdma_frame_base_time_`
  // is master_time of V=0 H=0 of the current frame (the anchor for line scheduling).
  HdmaPhase hdma_phase_ = HdmaPhase::kInit;
  uint32_t hdma_next_v_ = 0;
  uint8_t hdma_active_mask_ = 0;
  TimeMasterT hdma_frame_base_time_ = 0;
};

}  // namespace pupsnes
