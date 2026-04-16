#include "pupsnes/hw/snes.h"

#include <spdlog/spdlog.h>

#include "pupsnes/hw/device.h"
#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/systembus.h"

// --- Device ---

pupsnes::Device::Device(SNES* snes) : snes(snes) {
    if (snes != nullptr) {
        device_id_ = snes->registerDevice(this);
    }
}

// --- SNES ---

pupsnes::SNES::SNES() : scheduler(std::make_unique<Scheduler>(this)), system_bus(std::make_unique<SystemBus>(this)) {}
pupsnes::SNES::~SNES() = default;

pupsnes::device_id_t pupsnes::SNES::registerDevice(Device* device) {
    auto id = static_cast<device_id_t>(devices_.size());
    devices_.push_back(device);
    return id;
}

pupsnes::Device* pupsnes::SNES::getDevice(device_id_t id) const {
    if (id >= devices_.size()) {
        return nullptr;
    }
    return devices_[id];
}

void pupsnes::SNES::debugPrintInfo() {
    spdlog::debug("SNES Info:");
    spdlog::debug("  Time (master): {}", time_now);

    scheduler->debugPrintNextEvent();
    scheduler->debugPrintEventQueue();
}
