#include <cstdint>
#include <cstdio>

#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/types.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)

int main(int /*argc*/, char** /*argv*/) {
    std::printf("PupSNES");

    SNES snes;
    snes.DebugPrintInfo();

    snes.scheduler->ScheduleEvent(1000, nullptr, SchedulerPhase::kRun, EventType::kDeviceRun);
    snes.scheduler->Step();

    return 0;
}
