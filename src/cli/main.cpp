#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/types.h"

#include <cstdint>
#include <cstdio>

using namespace pupsnes;

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    SNES snes;
    snes.debugPrintInfo();

    snes.scheduler->scheduleEvent(1000, nullptr, SchedulerPhase::Run, EventType::DeviceRun);
    snes.scheduler->step();

    return 0;
}
