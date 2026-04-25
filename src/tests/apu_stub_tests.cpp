#include <catch2/catch_test_macros.hpp>

#include "pupsnes/hw/apu_stub.h"
#include "pupsnes/hw/snes.h"

using pupsnes::ApuStub;
using pupsnes::SNES;
using pupsnes::TimeMasterT;

TEST_CASE("APU stub does not present the IPL signature on the very first read",
          "[unit][apu]") {
  // Real SPC700 IPL ROM takes a few thousand master cycles after reset before
  // it writes $AA/$BB into ports 0/1. SMW's reset code spins on a 16-bit
  // `CMP $2140` loop waiting for that signature. If the stub returns $AA/$BB
  // immediately, the loop exits on the first iteration and the entire SPC700
  // boot window collapses, throwing trace alignment with reference emulators
  // off by hundreds of microseconds. Instead, the stub must hold the ports at
  // $00 until a small delay has elapsed.
  SNES snes;
  ApuStub& apu = snes.GetApuStub();
  apu.Reset();

  const auto t0 = static_cast<TimeMasterT>(0);
  const auto port0_at_zero = apu.ReadRegister(0x2140, t0).value;
  const auto port1_at_zero = apu.ReadRegister(0x2141, t0).value;

  REQUIRE(port0_at_zero == 0x00);
  REQUIRE(port1_at_zero == 0x00);
}

TEST_CASE("APU stub presents the IPL signature once the boot delay has elapsed",
          "[unit][apu]") {
  SNES snes;
  ApuStub& apu = snes.GetApuStub();
  apu.Reset();

  const auto t = static_cast<TimeMasterT>(ApuStub::kSignatureDelayMaster);
  const auto port0 = apu.ReadRegister(0x2140, t).value;
  const auto port1 = apu.ReadRegister(0x2141, t).value;

  REQUIRE(port0 == ApuStub::kResetPort0);
  REQUIRE(port1 == ApuStub::kResetPort1);
}
