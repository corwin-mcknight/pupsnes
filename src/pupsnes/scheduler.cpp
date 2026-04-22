#include "pupsnes/hw/scheduler.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "pupsnes/hw/snes.h"

namespace pupsnes {

Scheduler::Scheduler(SNES* snes) : snes_(snes) {}
Scheduler::~Scheduler() = default;

void Scheduler::Reset() {
  SignalHeap empty;
  std::swap(signal_queue_, empty);
  next_signal_seq_ = 0;
  token_table_ = TokenTable{};
}

void Scheduler::ScheduleSignal(TimeMasterT master_time, SignalKind kind, SignalEventHandler handler) {
  signal_queue_.push(SignalEvent{master_time, kind, std::move(handler), next_signal_seq_++});
}

TimeMasterT Scheduler::NextEventMasterTime() const {
  if (signal_queue_.empty()) {
    return std::numeric_limits<TimeMasterT>::max();
  }
  return signal_queue_.top().master_time;
}

void Scheduler::FireEventsThrough(TimeMasterT through_time) {
  while (!signal_queue_.empty() && signal_queue_.top().master_time <= through_time) {
    // priority_queue only exposes const top; move out via cast so the handler
    // function (which may own captures) is taken rather than copied.
    SignalEvent event = std::move(const_cast<SignalEvent&>(signal_queue_.top()));
    signal_queue_.pop();
    if (event.handler) {
      event.handler(event.master_time);
    }
  }
}

std::vector<SignalEventView> Scheduler::SnapshotSignalQueue() const {
  SignalHeap temp = signal_queue_;
  std::vector<SignalEventView> out;
  out.reserve(temp.size());
  while (!temp.empty()) {
    const SignalEvent& e = temp.top();
    out.push_back({e.master_time, e.kind});
    temp.pop();
  }
  return out;
}

TokenIdT Scheduler::CreateToken(const TokenCreateParams& params) { return token_table_.Create(params); }
const Token* Scheduler::GetToken(TokenIdT id) const { return token_table_.Get(id); }
void Scheduler::RemoveToken(TokenIdT id) { token_table_.Remove(id); }

}  // namespace pupsnes
