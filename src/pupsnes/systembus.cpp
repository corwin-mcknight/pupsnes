#include "pupsnes/hw/systembus.h"

#include <cstdio>

#include "pupsnes/config.h"
#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/mem.h"

namespace pupsnes {

SystemBus::SystemBus(SNES* snes) : snes_(snes) {}

void SystemBus::MapPage(const PageMapParams& params) {
    PageTableEntry& entry = page_table_[params.bank][params.page];
    entry.device_id = params.device;
    entry.base_offset = params.base_offset;
    entry.kind = params.kind;
    entry.access_speed = params.access_speed;
}

void SystemBus::UnmapPage(uint8_t bank, uint8_t page) { page_table_[bank][page] = PageTableEntry{}; }

BusPlan SystemBus::Plan(SnesAddrT address, BusAccessType type, uint8_t write_data) const {
    SnesAddrT addr = util::WrapAddr(address);
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
        case BusPlanOutcome::kInlineComplete:
            return FollowInline(plan, current_time);
        case BusPlanOutcome::kScheduledComplete:
            return FollowScheduled(plan, current_time, source_device);
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
            return {BusPlanOutcome::kRejected, last_data_bus_value_, 0};
    }
    return {BusPlanOutcome::kRejected, last_data_bus_value_, 0};
}

BusFollowResult SystemBus::FollowInline(const BusPlan& plan, TimeMasterT current_time) {
    Device* device = snes_->GetDevice(plan.target_device);
    if (device == nullptr) {
        return {BusPlanOutcome::kRejected, 0, 0};
    }

    const PageTableEntry& entry = page_table_[static_cast<uint8_t>(plan.original_address >> 16)]
                                             [static_cast<uint8_t>((plan.original_address >> 8) & 0xFF)];

    if (entry.kind == PageDeviceKind::kSameClockMmio) {
        snes_->scheduler->CatchUpDevice(plan.target_device, current_time);
    }

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
