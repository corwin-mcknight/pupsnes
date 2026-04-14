#pragma once

#include "pupsnes/hw/systembus.h"

namespace pupsnes {

struct SystemBusTestAccess {
    static const PageTableEntry &getPageEntry(const SystemBus &bus, uint8_t bank, uint8_t page) {
        return bus.page_table_[bank][page];
    }
};

} // namespace pupsnes
