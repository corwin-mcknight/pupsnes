#include <catch2/catch_test_macros.hpp>

#include "pupsnes/debugger/breakpoints.h"

using namespace pupsnes::debugger;  // NOLINT(google-build-using-namespace)

TEST_CASE("BreakpointSet add remove and hit detection works", "[unit][debugger]") {
  BreakpointSet breakpoints;

  REQUIRE_FALSE(breakpoints.Contains(0x008000));
  REQUIRE_FALSE(breakpoints.IsEnabled(0x008000));

  breakpoints.Set(0x008000);
  REQUIRE(breakpoints.Contains(0x008000));
  REQUIRE(breakpoints.IsEnabled(0x008000));

  breakpoints.Set(0x00C000, false);
  REQUIRE(breakpoints.Contains(0x00C000));
  REQUIRE_FALSE(breakpoints.IsEnabled(0x00C000));

  breakpoints.Toggle(0x008000);
  REQUIRE_FALSE(breakpoints.Contains(0x008000));

  breakpoints.Toggle(0x008100);
  REQUIRE(breakpoints.IsEnabled(0x008100));

  breakpoints.Remove(0x00C000);
  REQUIRE_FALSE(breakpoints.Contains(0x00C000));

  const auto snapshot = breakpoints.Snapshot();
  REQUIRE(snapshot.size() == 1);
  REQUIRE(snapshot.front().address == 0x008100);
  REQUIRE(snapshot.front().enabled);
}
