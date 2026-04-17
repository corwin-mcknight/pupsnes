#pragma once

#include <map>
#include <vector>

#include "pupsnes/types.h"

namespace pupsnes::debugger {

struct Breakpoint {
  SnesAddrT address = 0;
  bool enabled = true;
};

class BreakpointSet {
 public:
  void Set(SnesAddrT address, bool enabled = true);
  void Remove(SnesAddrT address);
  void Toggle(SnesAddrT address);
  [[nodiscard]] bool Contains(SnesAddrT address) const;
  [[nodiscard]] bool IsEnabled(SnesAddrT address) const;
  [[nodiscard]] std::vector<Breakpoint> Snapshot() const;
  void Clear();

 private:
  std::map<SnesAddrT, Breakpoint> breakpoints_;
};

}  // namespace pupsnes::debugger
