#pragma once

#include "pupsnes/types.h"
#include <memory>

namespace pupsnes {
class Scheduler;

// SNES holds all hardware components. A SNES effectively IS an emulated SNES.
class SNES {
    // Time
  public:
    time_master_t time_now = 0;
    time_apu_t time_apu_now = 0;
    std::unique_ptr<Scheduler> scheduler;

    SNES();
    ~SNES();

    void debugPrintInfo();

    time_master_t getMasterTime() const { return time_now; }
    void setMasterTime(time_master_t t) { time_now = t; }
};
} // namespace pupsnes
