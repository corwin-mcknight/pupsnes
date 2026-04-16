#pragma once

#include <array>
#include <cstdint>

#include "pupsnes/types.h"

namespace pupsnes {

class SNES;

enum class PageDeviceKind : uint8_t {
    Unmapped = 0,
    Memory = 1,
    SameClockMMIO = 2,
    CrossClockMMIO = 3,
};

struct PageTableEntry {
    device_id_t device_id = 0;
    uint32_t base_offset = 0;
    PageDeviceKind kind = PageDeviceKind::Unmapped;
    uint8_t access_speed = 0;
};

enum class BusPlanOutcome : uint8_t {
    InlineComplete = 0,
    ScheduledComplete = 1,
    Rejected = 2,
};

enum class BusAccessType : uint8_t {
    Read = 0,
    Write = 1,
};

struct BusPlan {
    BusPlanOutcome outcome;
    BusAccessType access_type;
    device_id_t target_device;
    uint32_t device_offset;
    snes_addr_t original_address;
    time_master_delta_t access_cycles;
    uint8_t write_data;
};

struct BusFollowResult {
    BusPlanOutcome outcome;
    uint8_t data;
    token_id_t token;
};

struct PageMapParams {
    uint8_t bank;
    uint8_t page;
    device_id_t device;
    uint32_t base_offset;
    PageDeviceKind kind;
    uint8_t access_speed;
};

class SystemBus {
   public:
    explicit SystemBus(SNES* snes);
    ~SystemBus() = default;

    void mapPage(const PageMapParams& params);
    void unmapPage(uint8_t bank, uint8_t page);

    [[nodiscard]] BusPlan plan(snes_addr_t address, BusAccessType type, uint8_t write_data = 0) const;
    BusFollowResult follow(const BusPlan& plan, time_master_t current_time, device_id_t source_device);

   private:
    SNES* snes_;  // Non-owning. SNES owns this SystemBus; pointer back to parent.
    uint8_t last_data_bus_value_ = 0xFF;
    using PageRow = std::array<PageTableEntry, 256>;
    std::array<PageRow, 256> page_table_{};

    BusFollowResult followInline(const BusPlan& plan, time_master_t current_time);
    BusFollowResult followScheduled(const BusPlan& plan, time_master_t current_time, device_id_t source_device);

    friend struct SystemBusTestAccess;
};

}  // namespace pupsnes
