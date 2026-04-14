#pragma once

#include "pupsnes/types.h"
#include <memory>

namespace pupsnes {
class Scheduler;

class SNES {
  public:
    std::unique_ptr<Scheduler> scheduler;

    SNES();
    ~SNES();

    void debugPrintInfo();

    [[nodiscard]] time_master_t getMasterTime() const { return time_now; }
    void setMasterTime(time_master_t t) { time_now = t; }

  private:
    time_master_t time_now = 0;
    // time_apu_t time_apu_now = 0;
};
} // namespace pupsnes
