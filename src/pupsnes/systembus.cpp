#include "pupsnes/hw/systembus.h"

#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/mem.h"

#include <cstring>

namespace pupsnes {

SystemBus::SystemBus(SNES *snes) : snes_(snes) { std::memset(page_table_, 0, sizeof(page_table_)); }

SystemBus::~SystemBus() = default;

void SystemBus::mapPage(uint8_t bank, uint8_t page, device_id_t device, uint32_t base_offset,
                        PageDeviceKind kind, uint8_t access_speed) {
    PageTableEntry &entry = page_table_[bank][page];
    entry.device_id = device;
    entry.base_offset = base_offset;
    entry.kind = kind;
    entry.access_speed = access_speed;
}

void SystemBus::unmapPage(uint8_t bank, uint8_t page) {
    page_table_[bank][page] = PageTableEntry{};
}

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

BusFollowResult SystemBus::follow(const BusPlan &plan, time_master_t current_time,
                                  device_id_t source_device) {
    switch (plan.outcome) {
    case BusPlanOutcome::InlineComplete:
        return followInline(plan, current_time);
    case BusPlanOutcome::ScheduledComplete:
        return followScheduled(plan, current_time, source_device);
    case BusPlanOutcome::Rejected:
        return {BusPlanOutcome::Rejected, 0, 0};
    }
    return {BusPlanOutcome::Rejected, 0, 0};
}

BusFollowResult SystemBus::followInline(const BusPlan &plan, time_master_t current_time) {
    Device *device = snes_->getDevice(plan.target_device);
    if (device == nullptr) {
        return {BusPlanOutcome::Rejected, 0, 0};
    }

    const PageTableEntry &entry =
        page_table_[static_cast<uint8_t>(plan.original_address >> 16)]
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

    return result;
}

BusFollowResult SystemBus::followScheduled(const BusPlan &plan, time_master_t current_time,
                                           device_id_t source_device) {
    TokenType token_type =
        (plan.access_type == BusAccessType::Read) ? TokenType::BusRead : TokenType::BusWrite;

    time_master_t completion_time = current_time + plan.access_cycles;

    token_id_t token_id = snes_->scheduler->createToken(token_type, source_device, completion_time,
                                                        plan.original_address, plan.write_data);

    return {BusPlanOutcome::ScheduledComplete, 0, token_id};
}

} // namespace pupsnes
