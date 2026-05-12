#include "pupsnes/hw/5a22/cpu_mmio.h"

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/cartridge.h"
#include "pupsnes/hw/dma_controller.h"
#include "pupsnes/hw/joypad.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/hw/systembus.h"

namespace pupsnes {

namespace {

// Pages $40-$43 cover both the legacy joypad serial ports ($4016/$4017 on
// page $40) and the standard 5A22 MMIO block ($4200-$43FF). Mapping the whole
// span keeps stray reads in this region from falling through to "unmapped" on
// the bus log even though most addresses inside $40-$41 are unimplemented.
constexpr uint8_t kFirstMmioPage = 0x40U;
constexpr uint8_t kLastMmioPage = 0x43U;

void MapMmioBank(SystemBus& bus, DeviceIdT device_id, uint8_t bank) {
  for (uint16_t page = kFirstMmioPage; page <= kLastMmioPage; ++page) {
    const uint32_t base_offset = static_cast<uint32_t>(page) * 0x100U;
    bus.MapPage({bank, static_cast<uint8_t>(page), device_id, base_offset, PageDeviceKind::kSameClockMmio, 8, nullptr,
                 nullptr});
  }
}

}  // namespace

CpuMmio::CpuMmio(SNES* snes) : Device(snes) {}

void CpuMmio::Reset() {
  const bool was_fast = (memsel_ & 0x01U) != 0U;
  memsel_ = 0;
  nmitimen_ = 0;
  // Mirror the write path: only rebuild the page table when FASTROM was
  // actually on. No-op remap on a cold machine where both state and bus
  // already agree.
  if (was_fast && snes_ != nullptr && snes_->cartridge != nullptr && snes_->system_bus != nullptr) {
    snes_->cartridge->OnMemSelChanged(*snes_->system_bus, false);
  }
}

void CpuMmio::MapSystemBus(SystemBus& bus) {
  for (uint8_t bank_base : {uint8_t{0x00U}, uint8_t{0x80U}}) {
    for (uint8_t bank_offset = 0; bank_offset < 0x40U; ++bank_offset) {
      MapMmioBank(bus, GetDeviceId(), static_cast<uint8_t>(bank_base + bank_offset));
    }
  }
}

MmioReadResult CpuMmio::ReadRegister(uint32_t offset, TimeMasterT current_time) {
  const uint32_t reg = offset & 0xFFFFU;
  if (reg == kMemSelOffset) {
    return {memsel_, 0xFFU};
  }
  if (reg == kRdNmiOffset) {
    // RDNMI read both samples and clears the VBlank NMI latch. Polling loops
    // of the form `BIT $4210 / BPL` see bit 7 high once per frame at VBlank
    // entry and fall back to 0 on the next read until the latch re-arms.
    bool vblank_nmi = false;
    if (snes_ != nullptr && snes_->ppu != nullptr) {
      vblank_nmi = snes_->ppu->QueryAndClearVblankNmiFlag(current_time);
    }
    uint8_t value = kRdNmiCpuVersion;
    if (vblank_nmi) value = static_cast<uint8_t>(value | kRdNmiVblankFlagMask);
    return {value, kRdNmiDrivenMask};
  }
  if (reg == kHvbJoyOffset) {
    // Query the PPU directly so the flags reflect the bus cycle's master time.
    // The PPU catches up internally; missing the call would leak stale h/v
    // from whenever the PPU last advanced.
    PpuHvbStatus status{false, false};
    if (snes_ != nullptr && snes_->ppu != nullptr) {
      status = snes_->ppu->QueryHvbStatus(current_time);
    }
    uint8_t value = 0;
    if (status.vblank) value = static_cast<uint8_t>(value | kHvbJoyVblankMask);
    if (status.hblank) value = static_cast<uint8_t>(value | kHvbJoyHblankMask);
    // Auto-joypad busy bit stays 0 until the joypad auto-read controller lands.
    return {value, kHvbJoyDrivenMask};
  }
  if (reg == kJoySer0Offset) {
    // Manual serial port for P1. Only bit 0 carries pad data; leave bits 7-1
    // as open-bus rather than fabricating zeros — real hardware exposes a few
    // open I/O pins in that window.
    if (snes_ != nullptr && snes_->joypad != nullptr) {
      return {snes_->joypad->ReadJoySer0(), 0x01U};
    }
    return {0x00U, 0x01U};
  }
  if (reg == kJoySer1Offset) {
    // P2 manual serial port. No P2 controller, so the data line reads 0 with
    // bit 0 driven; bits 7-1 stay open-bus.
    if (snes_ != nullptr && snes_->joypad != nullptr) {
      return {snes_->joypad->ReadJoySer1(), 0x01U};
    }
    return {0x00U, 0x01U};
  }
  if (reg == kAutoJoyResultFirst) {
    // $4218 JOY1L — A, X, L, R in bits 7..4, controller-type ID in bits 3..0.
    const uint8_t value = (snes_ != nullptr && snes_->joypad != nullptr) ? snes_->joypad->ReadJoy1L() : 0x00U;
    return {value, 0xFFU};
  }
  if (reg == kAutoJoyResultFirst + 1U) {
    // $4219 JOY1H — B, Y, Select, Start, Up, Down, Left, Right.
    const uint8_t value = (snes_ != nullptr && snes_->joypad != nullptr) ? snes_->joypad->ReadJoy1H() : 0x00U;
    return {value, 0xFFU};
  }
  if (reg > kAutoJoyResultFirst + 1U && reg <= kAutoJoyResultLast) {
    // $421A-$421F: JOY2/JOY3/JOY4. No P2-P4 controllers; drive zero so polling
    // doesn't pick up open-bus garbage as phantom button presses.
    return {0x00U, 0xFFU};
  }
  // Stub: other CPU MMIO registers (NMITIMEN, RDNMI, HDMA) are not yet
  // modeled. Return pure open-bus (mask=0) so the bus merges in the last data
  // value instead of a hard zero.
  return {0x00U, 0x00U};
}

void CpuMmio::WriteRegister(uint32_t offset, uint8_t data, TimeMasterT current_time) {
  const uint32_t reg = offset & 0xFFFFU;
  if (reg == kMemSelOffset) {
    const bool was_fast = (memsel_ & 0x01U) != 0U;
    memsel_ = data;
    const bool now_fast = (memsel_ & 0x01U) != 0U;
    // Re-map the LoROM fast-bank pages only when the FASTROM bit actually
    // flipped. ROMs commonly re-poke $420D with the same value; avoid
    // reshuffling 126 banks * 128 pages of page-table entries on no-op writes.
    if (was_fast != now_fast && snes_ != nullptr && snes_->cartridge != nullptr && snes_->system_bus != nullptr) {
      snes_->cartridge->OnMemSelChanged(*snes_->system_bus, now_fast);
    }
    return;
  }
  if (reg == kNmiTimenOffset) {
    const uint8_t prev = nmitimen_;
    nmitimen_ = data;
    // NMI-enable transitions trigger the two NMITIMEN.7 quirks (0→1
    // transparency while /NMI is asserted → immediate NMI; 1→0 cancels any
    // pending NMI). The CPU owns the flip-flop and runs the transition
    // logic, since it's the one with the edge-tracker state.
    if (((prev ^ data) & kNmiTimenNmiEnableMask) != 0U && snes_ != nullptr && snes_->cpu != nullptr) {
      snes_->cpu->OnNmiTimenChanged(prev, data, current_time);
    }
    // V-IRQ / H-IRQ / auto-joypad bits stored but not acted upon in v1 —
    // IRQ signal and joypad auto-read land in subsequent work. Keeping the
    // shadow byte lets the debugger show the current enable state.
    return;
  }
  if (reg == kMdmaEnOffset) {
    if (data != 0U && snes_ != nullptr && snes_->dma != nullptr) {
      const TimeMasterT end_time = snes_->dma->Trigger(data, current_time);
      // Stall the CPU: bump master time and the CPU's local-time mirror so the
      // next bus access sees the post-DMA cycle. Real hardware halts the 65816
      // for the DMA duration; this models the same effect inline.
      snes_->SetMasterTime(end_time);
      if (snes_->cpu != nullptr) {
        snes_->cpu->SetLocalTime(end_time);
      }
    }
    return;
  }
  if (reg == kHdmaEnOffset) {
    // HDMA enable: shadow only in v1 (HDMA itself not yet implemented).
    hdmaen_ = data;
    return;
  }
  if (reg == kJoySer0Offset) {
    // $4016 write — bit 0 is the manual-serial strobe for both controller
    // ports. Bits 7-1 are programmable I/O pins on the controller connector;
    // ignored here because we don't model the I/O port.
    if (snes_ != nullptr && snes_->joypad != nullptr) {
      snes_->joypad->WriteJoySer0(data);
    }
    return;
  }
  // Stub: writes to other registers are accepted silently so ROMs can poke
  // them without the bus rejecting the transaction.
}

std::optional<uint8_t> CpuMmio::HandleDebugRead(uint32_t offset) const {
  const uint32_t reg = offset & 0xFFFFU;
  if (reg == kMemSelOffset) {
    return memsel_;
  }
  if (reg == kNmiTimenOffset) {
    // $4200 is write-only on real hardware; expose the shadow for the debugger.
    return nmitimen_;
  }
  return 0x00U;
}

bool CpuMmio::HandleDebugWrite(uint32_t offset, uint8_t data) {
  const uint32_t reg = offset & 0xFFFFU;
  if (reg == kMemSelOffset || reg == kNmiTimenOffset) {
    // Debug writes are out-of-band and don't belong to a bus cycle; pass 0
    // as the current time. CpuMmio commits synchronously, so the timestamp
    // is unused. Devices that use lazy replay (PPU) must not be debug-written
    // through this path — they provide their own HandleDebugWrite override.
    WriteRegister(reg, data, 0);
  }
  return true;
}

}  // namespace pupsnes
