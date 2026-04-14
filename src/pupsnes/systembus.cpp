#include "pupsnes/hw/systembus.h"

#include "pupsnes/config.h"
#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/mem.h"

#include <cstdio>

namespace pupsnes {

SystemBus::SystemBus(SNES *snes) : snes_(snes) {}

SystemBus::~SystemBus() = default;

void SystemBus::mapPage(const PageMapParams &params) {
    PageTableEntry &entry = page_table_[params.bank][params.page];
    entry.device_id = params.device;
    entry.base_offset = params.base_offset;
    entry.kind = params.kind;
    entry.access_speed = params.access_speed;
}

void SystemBus::unmapPage(uint8_t bank, uint8_t page) { page_table_[bank][page] = PageTableEntry{}; }

BusPlan SystemBus::plan(snes_addr_t address, BusAccessType type, uint8_t write_data) const {
    snes_addr_t addr = util::wrapAddr(address);
    uint8_t bank = static_cast<uint8_t>(addr >> 16);
    uint8_t page = static_cast<uint8_t>((addr >> 8) & 0xFF);
    uint8_t offset_in_page = static_cast<uint8_t>(addr & 0xFF);

    const PageTableEntry &entry = page_table_[bank][page];

    BusPlan plan{};
    plan.access_type = type;
    plan.original_address = addr;
    plan.write_data = write_data;

    if (entry.kind == PageDeviceKind::Unmapped) {
        plan.outcome = BusPlanOutcome::Rejected;
        return plan;
    }

    plan.target_device = entry.device_id;
    plan.device_offset = entry.base_offset + offset_in_page;
    plan.access_cycles = entry.access_speed;

    if (entry.kind == PageDeviceKind::CrossClockMMIO) {
        plan.outcome = BusPlanOutcome::ScheduledComplete;
    } else {
        plan.outcome = BusPlanOutcome::InlineComplete;
    }

    return plan;
}

BusFollowResult SystemBus::follow(const BusPlan &plan, time_master_t current_time, device_id_t source_device) {
    switch (plan.outcome) {
    case BusPlanOutcome::InlineComplete:
        return followInline(plan, current_time);
    case BusPlanOutcome::ScheduledComplete:
        return followScheduled(plan, current_time, source_device);
    case BusPlanOutcome::Rejected:
        if constexpr (config::kLogUnmappedBusAccess) {
            if (plan.access_type == BusAccessType::Read) {
                std::fprintf(stderr, "[BUS] unmapped read  $%02X:%04X -> open-bus 0x%02X\n",
                             static_cast<unsigned>(plan.original_address >> 16),
                             static_cast<unsigned>(plan.original_address & 0xFFFF), last_data_bus_value_);
            } else {
                std::fprintf(stderr, "[BUS] unmapped write $%02X:%04X <- 0x%02X (dropped)\n",
                             static_cast<unsigned>(plan.original_address >> 16),
                             static_cast<unsigned>(plan.original_address & 0xFFFF), plan.write_data);
            }
        }
        return {BusPlanOutcome::Rejected, last_data_bus_value_, 0};
    }
    return {BusPlanOutcome::Rejected, last_data_bus_value_, 0};
}

BusFollowResult SystemBus::followInline(const BusPlan &plan, time_master_t current_time) {
    Device *device = snes_->getDevice(plan.target_device);
    if (device == nullptr) {
        return {BusPlanOutcome::Rejected, 0, 0};
    }

    const PageTableEntry &entry = page_table_[static_cast<uint8_t>(plan.original_address >> 16)]
                                             [static_cast<uint8_t>((plan.original_address >> 8) & 0xFF)];

    if (entry.kind == PageDeviceKind::SameClockMMIO) {
        snes_->scheduler->catchUpDevice(plan.target_device, current_time);
    }

    BusFollowResult result{};
    result.outcome = BusPlanOutcome::InlineComplete;
    result.token = 0;

    if (plan.access_type == BusAccessType::Read) {
        result.data = device->readRegister(plan.device_offset);
    } else {
        device->writeRegister(plan.device_offset, plan.write_data);
        result.data = plan.write_data;
    }

    last_data_bus_value_ = result.data;
    return result;
}

BusFollowResult SystemBus::followScheduled(const BusPlan &plan, time_master_t current_time, device_id_t source_device) {
    token_id_t token_id = snes_->scheduler->createToken({
        .type = (plan.access_type == BusAccessType::Read) ? TokenType::BusRead : TokenType::BusWrite,
        .source_device = source_device,
        .completion_time = current_time + plan.access_cycles,
        .address = plan.original_address,
        .data = plan.write_data,
    });

    return {BusPlanOutcome::ScheduledComplete, 0, token_id};
}

} // namespace pupsnes
