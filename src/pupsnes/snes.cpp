#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/scheduler.h"

pupsnes::SNES::SNES() : scheduler(std::make_unique<Scheduler>(this)) {}
pupsnes::SNES::~SNES() = default;

void pupsnes::SNES::debugPrintInfo() {
    printf("SNES Info:\n");
    printf("  Time (master): %llu\n", static_cast<unsigned long long>(time_now));
    printf("  Time (APU): %llu\n", static_cast<unsigned long long>(time_apu_now));

    scheduler->debugPrintNextEvent();
    scheduler->debugPrintEventQueue();
}