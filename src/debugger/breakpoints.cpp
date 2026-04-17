#include "pupsnes/debugger/breakpoints.h"

#include <vector>

namespace pupsnes::debugger {

void BreakpointSet::Set(SnesAddrT address, bool enabled) { breakpoints_[address] = Breakpoint{address, enabled}; }

void BreakpointSet::Remove(SnesAddrT address) { breakpoints_.erase(address); }

void BreakpointSet::Toggle(SnesAddrT address) {
  auto it = breakpoints_.find(address);
  if (it == breakpoints_.end()) {
    Set(address, true);
    return;
  }
  breakpoints_.erase(it);
}

bool BreakpointSet::Contains(SnesAddrT address) const { return breakpoints_.contains(address); }

bool BreakpointSet::IsEnabled(SnesAddrT address) const {
  const auto it = breakpoints_.find(address);
  return it != breakpoints_.end() && it->second.enabled;
}

std::vector<Breakpoint> BreakpointSet::Snapshot() const {
  std::vector<Breakpoint> snapshot;
  snapshot.reserve(breakpoints_.size());
  for (const auto& [_, breakpoint] : breakpoints_) {
    snapshot.push_back(breakpoint);
  }
  return snapshot;
}

void BreakpointSet::Clear() { breakpoints_.clear(); }

}  // namespace pupsnes::debugger
