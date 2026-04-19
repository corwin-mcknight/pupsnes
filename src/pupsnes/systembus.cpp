#include "pupsnes/hw/systembus.h"

#include <cstdio>

#include "pupsnes/config.h"
#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/mem.h"

namespace pupsnes {

SystemBus::SystemBus(SNES* snes) : snes_(snes) {}

namespace {

SnesAddrT NormalizeAddress(SnesAddrT address) { return util::WrapAddr(address); }

}  // namespace

void SystemBus::MapPage(const PageMapParams& params) {
  PageTableEntry& entry = page_table_[params.bank][params.page];
  entry.device_id = params.device;
  entry.base_offset = params.base_offset;
  entry.kind = params.kind;
  entry.access_speed = params.access_speed;
  entry.fast_read_ptr = params.fast_read_ptr;
  entry.fast_write_ptr = params.fast_write_ptr;
}

void SystemBus::UnmapPage(uint8_t bank, uint8_t page) { page_table_[bank][page] = PageTableEntry{}; }

BusPlan SystemBus::Plan(SnesAddrT address, BusAccessType type, uint8_t write_data) const {
  SnesAddrT addr = NormalizeAddress(address);
  uint8_t bank = static_cast<uint8_t>(addr >> 16);
  uint8_t page = static_cast<uint8_t>((addr >> 8) & 0xFF);
  uint8_t offset_in_page = static_cast<uint8_t>(addr & 0xFF);

  const PageTableEntry& entry = page_table_[bank][page];

  BusPlan plan{};
  plan.access_type = type;
  plan.original_address = addr;
  plan.write_data = write_data;

  if (entry.kind == PageDeviceKind::kUnmapped) {
    plan.outcome = BusPlanOutcome::kRejected;
    return plan;
  }

  plan.target_device = entry.device_id;
  plan.device_offset = entry.base_offset + offset_in_page;
  plan.access_cycles = entry.access_speed;

  if (entry.kind == PageDeviceKind::kCrossClockMmio) {
    plan.outcome = BusPlanOutcome::kScheduledComplete;
  } else {
    plan.outcome = BusPlanOutcome::kInlineComplete;
  }

  return plan;
}

BusFollowResult SystemBus::Follow(const BusPlan& plan, TimeMasterT current_time, DeviceIdT source_device) {
  switch (plan.outcome) {
    case BusPlanOutcome::kInlineComplete: {
      const BusFollowResult result = FollowInline(plan, current_time);
      if (event_sink_ != nullptr) {
        const BusEventKind kind =
            (plan.access_type == BusAccessType::kRead) ? BusEventKind::kInlineRead : BusEventKind::kInlineWrite;
        const uint8_t data = (plan.access_type == BusAccessType::kRead) ? result.data : plan.write_data;
        event_sink_->OnBusEvent({current_time, plan.original_address, data, kind});
      }
      return result;
    }
    case BusPlanOutcome::kScheduledComplete: {
      const BusFollowResult result = FollowScheduled(plan, current_time, source_device);
      if (event_sink_ != nullptr) {
        const BusEventKind kind =
            (plan.access_type == BusAccessType::kRead) ? BusEventKind::kScheduledRead : BusEventKind::kScheduledWrite;
        event_sink_->OnBusEvent({current_time, plan.original_address, plan.write_data, kind});
      }
      return result;
    }
    case BusPlanOutcome::kRejected:
      if constexpr (config::kLogUnmappedBusAccess) {
        if (plan.access_type == BusAccessType::kRead) {
          std::fprintf(stderr, "[BUS] unmapped read  $%02X:%04X -> open-bus 0x%02X\n",
                       static_cast<unsigned>(plan.original_address >> 16),
                       static_cast<unsigned>(plan.original_address & 0xFFFF), last_data_bus_value_);
        } else {
          std::fprintf(stderr, "[BUS] unmapped write $%02X:%04X <- 0x%02X (dropped)\n",
                       static_cast<unsigned>(plan.original_address >> 16),
                       static_cast<unsigned>(plan.original_address & 0xFFFF), plan.write_data);
        }
      }
      if (event_sink_ != nullptr) {
        const BusEventKind kind =
            (plan.access_type == BusAccessType::kRead) ? BusEventKind::kRejectedRead : BusEventKind::kRejectedWrite;
        const uint8_t data = (plan.access_type == BusAccessType::kRead) ? last_data_bus_value_ : plan.write_data;
        event_sink_->OnBusEvent({current_time, plan.original_address, data, kind});
      }
      return {BusPlanOutcome::kRejected, last_data_bus_value_, 0};
  }
  return {BusPlanOutcome::kRejected, last_data_bus_value_, 0};
}

void SystemBus::NotifyEvent(BusEventKind kind, SnesAddrT address, uint8_t data) {
  // Called from the hot fast-path after the caller verified event_sink_ is
  // non-null. Uses scheduler-committed master time, which lags the CPU's
  // in-flight cycle_time by up to a Tick's worth of work — acceptable for a
  // bus viewer (ordering is preserved; per-cycle precision is not).
  const TimeMasterT time = (snes_ != nullptr) ? snes_->GetMasterTime() : 0;
  event_sink_->OnBusEvent({time, address, data, kind});
}

DebugReadResult SystemBus::MakeDebugReadResult(const BusPlan& plan) const {
  DebugReadResult result{};
  result.address = plan.original_address;
  result.device_id = plan.target_device;
  result.device_offset = plan.device_offset;

  if (plan.outcome == BusPlanOutcome::kRejected) {
    result.failure = DebugAccessFailureKind::kUnmapped;
    return result;
  }

  Device* device = snes_->GetDevice(plan.target_device);
  if (device == nullptr) {
    result.failure = DebugAccessFailureKind::kDeviceUnavailable;
    return result;
  }

  const std::optional<uint8_t> value = device->HandleDebugRead(plan.device_offset);
  if (!value.has_value()) {
    result.failure = DebugAccessFailureKind::kDeviceRefused;
    return result;
  }

  result.ok = true;
  result.value = *value;
  result.failure = DebugAccessFailureKind::kNone;
  return result;
}

DebugWriteResult SystemBus::MakeDebugWriteResult(const BusPlan& plan, uint8_t value) const {
  DebugWriteResult result{};
  result.address = plan.original_address;
  result.device_id = plan.target_device;
  result.device_offset = plan.device_offset;
  result.value = value;

  if (plan.outcome == BusPlanOutcome::kRejected) {
    result.failure = DebugAccessFailureKind::kUnmapped;
    return result;
  }

  Device* device = snes_->GetDevice(plan.target_device);
  if (device == nullptr) {
    result.failure = DebugAccessFailureKind::kDeviceUnavailable;
    return result;
  }

  if (!device->HandleDebugWrite(plan.device_offset, value)) {
    result.failure = DebugAccessFailureKind::kDeviceRefused;
    return result;
  }

  result.ok = true;
  result.failure = DebugAccessFailureKind::kNone;
  return result;
}

DebugReadResult SystemBus::DebugRead(SnesAddrT address) const {
  const BusPlan plan = Plan(NormalizeAddress(address), BusAccessType::kRead);
  return MakeDebugReadResult(plan);
}

DebugWriteResult SystemBus::DebugWrite(SnesAddrT address, uint8_t value) {
  const BusPlan plan = Plan(NormalizeAddress(address), BusAccessType::kWrite, value);
  return MakeDebugWriteResult(plan, value);
}

BusFollowResult SystemBus::FollowInline(const BusPlan& plan, TimeMasterT current_time) {
  const PageTableEntry& entry = page_table_[static_cast<uint8_t>(plan.original_address >> 16)]
                                           [static_cast<uint8_t>((plan.original_address >> 8) & 0xFF)];
  const uint8_t offset_in_page = static_cast<uint8_t>(plan.original_address & 0xFF);

  // Fast path: pure storage with a direct pointer — skip virtual dispatch.
  // kMemory guarantees no side effects, no catch-up, no arbitration, so the
  // only work is the byte load/store and the open-bus update.
  if (entry.kind == PageDeviceKind::kMemory) {
    if (plan.access_type == BusAccessType::kRead) {
      if (entry.fast_read_ptr != nullptr) {
        const uint8_t data = entry.fast_read_ptr[offset_in_page];
        last_data_bus_value_ = data;
        return {BusPlanOutcome::kInlineComplete, data, 0};
      }
    } else if (entry.fast_write_ptr != nullptr) {
      entry.fast_write_ptr[offset_in_page] = plan.write_data;
      last_data_bus_value_ = plan.write_data;
      return {BusPlanOutcome::kInlineComplete, plan.write_data, 0};
    }
    // Fall through to device dispatch when no fast pointer was provided
    // (e.g. test devices that want observable ReadRegister / WriteRegister).
  }

  Device* device = snes_->GetDevice(plan.target_device);
  if (device == nullptr) {
    return {BusPlanOutcome::kRejected, 0, 0};
  }

  if (entry.kind == PageDeviceKind::kSameClockMmio) {
    snes_->scheduler->CatchUpDevice(plan.target_device, current_time);
  }
  // TODO: kArbitrated — when a contended mapper (SA-1 / SuperFX shared SRAM)
  // lands, arbitrate bus ownership here before dispatching to the device.

  BusFollowResult result{};
  result.outcome = BusPlanOutcome::kInlineComplete;
  result.token = 0;

  if (plan.access_type == BusAccessType::kRead) {
    result.data = device->ReadRegister(plan.device_offset);
  } else {
    device->WriteRegister(plan.device_offset, plan.write_data);
    result.data = plan.write_data;
  }

  last_data_bus_value_ = result.data;
  return result;
}

BusFollowResult SystemBus::FollowScheduled(const BusPlan& plan, TimeMasterT current_time, DeviceIdT source_device) {
  TokenIdT token_id = snes_->scheduler->CreateToken({
      .type = (plan.access_type == BusAccessType::kRead) ? TokenType::kBusRead : TokenType::kBusWrite,
      .source_device = source_device,
      .completion_time = current_time + plan.access_cycles,
      .address = plan.original_address,
      .data = plan.write_data,
  });

  return {BusPlanOutcome::kScheduledComplete, 0, token_id};
}

}  // namespace pupsnes
