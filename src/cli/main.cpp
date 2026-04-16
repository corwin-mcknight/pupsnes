#include <cstdint>
#include <cstdio>

#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/types.h"

using namespace pupsnes;

int main(int /*argc*/, char** /*argv*/) {
    std::printf("PupSNES");

    SNES snes;
    snes.debugPrintInfo();

    snes.scheduler->scheduleEvent(1000, nullptr, SchedulerPhase::Run, EventType::DeviceRun);
    snes.scheduler->step();

    return 0;
}
