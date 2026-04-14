#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/scheduler.h"

#include <spdlog/spdlog.h>

pupsnes::SNES::SNES() : scheduler(std::make_unique<Scheduler>(this)) {}
pupsnes::SNES::~SNES() = default;

void pupsnes::SNES::debugPrintInfo() {
    spdlog::debug("SNES Info:");
    spdlog::debug("  Time (master): {}", time_now);
    // spdlog::debug("  Time (APU): {}", time_apu_now);

    scheduler->debugPrintNextEvent();
    scheduler->debugPrintEventQueue();
}
