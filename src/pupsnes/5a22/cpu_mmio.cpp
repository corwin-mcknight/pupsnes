#include "pupsnes/hw/5a22/cpu_mmio.h"

#include "pupsnes/hw/cartridge.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"

namespace pupsnes {

namespace {

constexpr uint8_t kFirstMmioPage = 0x42U;
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

TickResult CpuMmio::Tick(TimeMasterDeltaT budget) {
  // CpuMmio has no background work — it only reacts to bus accesses. Returning
  // kNoWork with no wake time tells HandleRunResult to clear the pending run
  // instead of re-scheduling. Critically, kSameClockMmio catch-up calls Tick
  // on us every time the CPU pokes $4200-$43FF; kBudgetExhausted would make
  // the scheduler enqueue a fresh DeviceRun at the caught-up time, which
  // later dispatches with a 0-byte budget and trips the zero-progress guard.
  return {budget, TickStopReason::kNoWork};
}

void CpuMmio::OnEvent(const SchedulerEvent& /*event*/) {}

uint8_t CpuMmio::ReadRegister(uint32_t offset) {
  const uint32_t reg = offset & 0xFFFFU;
  if (reg == kMemSelOffset) {
    return memsel_;
  }
  // Stub: other CPU MMIO registers (NMITIMEN, RDNMI, joypad, HDMA) are not yet
  // modeled. Return open-bus sentinel; callers that require real values must
  // wait for the corresponding follow-up task.
  return 0x00U;
}

void CpuMmio::WriteRegister(uint32_t offset, uint8_t data) {
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
  }
  // Stub: writes to other registers are accepted silently so ROMs can poke
  // them without the bus rejecting the transaction.
}

std::optional<uint8_t> CpuMmio::HandleDebugRead(uint32_t offset) const {
  const uint32_t reg = offset & 0xFFFFU;
  if (reg == kMemSelOffset) {
    return memsel_;
  }
  return 0x00U;
}

bool CpuMmio::HandleDebugWrite(uint32_t offset, uint8_t data) {
  const uint32_t reg = offset & 0xFFFFU;
  if (reg == kMemSelOffset) {
    WriteRegister(reg, data);
  }
  return true;
}

}  // namespace pupsnes
