#include "pupsnes/hw/snes.h"

#include <format>
#include <iostream>

#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/systembus.h"

// --- Device ---

pupsnes::Device::Device(SNES* snes) : snes_(snes) {
    if (snes_ != nullptr) {
        device_id_ = snes_->RegisterDevice(this);
    }
}

// --- SNES ---

pupsnes::SNES::SNES() : scheduler(std::make_unique<Scheduler>(this)), system_bus(std::make_unique<SystemBus>(this)) {}
pupsnes::SNES::~SNES() = default;

pupsnes::DeviceIdT pupsnes::SNES::RegisterDevice(Device* device) {
    auto id = static_cast<DeviceIdT>(devices_.size());
    devices_.push_back(device);
    return id;
}

pupsnes::Device* pupsnes::SNES::GetDevice(DeviceIdT id) const {
    if (id >= devices_.size()) {
        return nullptr;
    }
    return devices_[id];
}

void pupsnes::SNES::DebugPrintInfo() {
    std::cerr << "SNES Info:\n";
    std::cerr << std::format("  Time (master): {}\n", time_now_);

    scheduler->DebugPrintNextEvent();
    scheduler->DebugPrintEventQueue();
}
