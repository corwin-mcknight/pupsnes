#pragma once

#include "pupsnes/hw/systembus.h"

namespace pupsnes {

struct SystemBusTestAccess {
    static const PageTableEntry& getPageEntry(const SystemBus& bus, std::size_t bank, std::size_t page) {
        return bus.page_table_[bank][page];
    }
};

}  // namespace pupsnes
