#include "pupsnes/debugger/console_event_sink.h"

#include "pupsnes/debugger/emu_event_format.h"

namespace pupsnes::debugger {

void ConsoleEventSink::OnEmuEvent(const EmuEvent& event) {
  if ((category_mask_ & EmuEventCategoryBit(EmuEventCategoryOf(event.kind))) == 0U) {
    return;
  }
  (*out_) << FormatEmuEventLine(event) << '\n';
}

}  // namespace pupsnes::debugger
