#include <catch2/catch_test_macros.hpp>

#include "pupsnes/debugger/error_log.h"

using namespace pupsnes;            // NOLINT(google-build-using-namespace)
using namespace pupsnes::debugger;  // NOLINT(google-build-using-namespace)

TEST_CASE("ErrorLog records debugger-facing event types and evicts oldest entries", "[unit][debugger]") {
  ErrorLog log(4);

  CPU::Fault fault{};
  fault.opcode = 0x00;
  fault.opcode_address = 0x008123;
  fault.regs.PC = 0x8124;
  log.PushCpuFault(12, fault);

  log.PushBusAccessFailure(13, 0x7E0010, "Unmapped read at $7E:0010");
  log.PushSchedulerError(14, "Scheduler event in the past");

  DebugWriteResult debug_write_refusal{};
  debug_write_refusal.address = 0x002100;
  debug_write_refusal.device_id = 7;
  debug_write_refusal.failure = DebugAccessFailureKind::kDeviceRefused;
  log.PushDebugWriteRefusal(15, debug_write_refusal);

  const auto entries = log.Snapshot();
  REQUIRE(entries.size() == 4);
  REQUIRE(entries[0].source == ErrorSource::kCpu);
  REQUIRE(entries[1].source == ErrorSource::kBus);
  REQUIRE(entries[2].source == ErrorSource::kScheduler);
  REQUIRE(entries[3].source == ErrorSource::kBus);
  REQUIRE(entries[3].address == 0x002100);

  log.PushHostError(16, "GLFW init failed");

  const auto evicted = log.Snapshot();
  REQUIRE(evicted.size() == 4);
  REQUIRE(evicted.front().master_time == 13);
  REQUIRE(evicted.back().source == ErrorSource::kHost);
}
