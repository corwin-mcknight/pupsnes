#include <catch2/catch_test_macros.hpp>

#include "pupsnes/debugger/fan_out_trace_sink.h"
#include "pupsnes/core/debugger_contract.h"

using pupsnes::TraceEntry;
using pupsnes::TraceSink;
using pupsnes::debugger::FanOutTraceSink;

namespace {
class CountingSink : public TraceSink {
 public:
  void Record(const TraceEntry& /*e*/) override { ++count; }
  int count = 0;
};
}  // namespace

TEST_CASE("FanOutTraceSink fans out to every attached sink", "[unit][debugger]") {
  FanOutTraceSink fan_out;
  CountingSink a;
  CountingSink b;
  fan_out.Attach(&a);
  fan_out.Attach(&b);

  TraceEntry entry{};
  fan_out.Record(entry);
  fan_out.Record(entry);

  CHECK(a.count == 2);
  CHECK(b.count == 2);
  CHECK(fan_out.Count() == 2);
}

TEST_CASE("FanOutTraceSink Attach is idempotent", "[unit][debugger]") {
  FanOutTraceSink fan_out;
  CountingSink a;
  fan_out.Attach(&a);
  fan_out.Attach(&a);

  TraceEntry entry{};
  fan_out.Record(entry);

  CHECK(a.count == 1);
  CHECK(fan_out.Count() == 1);
}

TEST_CASE("FanOutTraceSink Detach stops dispatch to that sink", "[unit][debugger]") {
  FanOutTraceSink fan_out;
  CountingSink a;
  CountingSink b;
  fan_out.Attach(&a);
  fan_out.Attach(&b);
  fan_out.Detach(&a);

  TraceEntry entry{};
  fan_out.Record(entry);

  CHECK(a.count == 0);
  CHECK(b.count == 1);
  CHECK(fan_out.Count() == 1);
}

TEST_CASE("FanOutTraceSink Detach of unknown pointer is a no-op", "[unit][debugger]") {
  FanOutTraceSink fan_out;
  CountingSink a;
  CountingSink other;
  fan_out.Attach(&a);

  fan_out.Detach(&other);  // must not crash

  TraceEntry entry{};
  fan_out.Record(entry);
  CHECK(a.count == 1);
}
