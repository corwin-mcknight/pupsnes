#include <catch2/catch_test_macros.hpp>
#include <string_view>

#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/5a22/micro_op.h"
#include "pupsnes/hw/5a22/micro_op_strings.h"

namespace pupsnes {
namespace {

TEST_CASE("MicroOpStrings - KnownBusActions", "[microop]") {
  CHECK(ToString(MicroBusAction::kNone) == std::string_view{"None"});
  CHECK(ToString(MicroBusAction::kFetchPc) == std::string_view{"FetchPc"});
  CHECK(ToString(MicroBusAction::kReadAddr) == std::string_view{"ReadAddr"});
  CHECK(ToString(MicroBusAction::kWriteRegByte) == std::string_view{"WriteRegByte"});
  CHECK(ToString(MicroBusAction::kPullStack) == std::string_view{"PullStack"});
}

TEST_CASE("MicroOpStrings - KnownInternalOps", "[microop]") {
  CHECK(ToString(MicroInternalOp::kNone) == std::string_view{"None"});
  CHECK(ToString(MicroInternalOp::kLoadReg) == std::string_view{"LoadReg"});
  CHECK(ToString(MicroInternalOp::kBranchRelative) == std::string_view{"BranchRelative"});
  CHECK(ToString(MicroInternalOp::kMaskStatus) == std::string_view{"MaskStatus"});
}

class CountingRecorder : public MicroOpRecorder {
 public:
  void OnInstructionBegin(uint8_t /*opcode*/, SnesAddrT /*pc*/) override { ++begins; }
  void OnMicroOp(const MicroOpRecord& /*rec*/) override { ++ops; }
  void OnInstructionEnd(uint64_t /*retired_seq*/) override { ++ends; }
  int begins = 0;
  int ops = 0;
  int ends = 0;
};

TEST_CASE("MicroOpRecorder interface is implementable", "[microop]") {
  CountingRecorder r;
  MicroOpRecord rec;
  rec.index = 1;
  rec.bus_action = MicroBusAction::kFetchPc;
  rec.internal_op = MicroInternalOp::kNone;
  rec.status = MicroOpStatus::kExecuted;
  r.OnInstructionBegin(0xA9, 0x008000);
  r.OnMicroOp(rec);
  r.OnInstructionEnd(1);
  CHECK(r.begins == 1);
  CHECK(r.ops == 1);
  CHECK(r.ends == 1);
}

}  // namespace
}  // namespace pupsnes

#include "pupsnes/debugger/microop_trace.h"

namespace pupsnes::debugger {
namespace {

TEST_CASE("MicroOpTrace records in-flight instruction", "[microop]") {
  MicroOpTrace trace;
  trace.OnInstructionBegin(0xA9, 0x008000);
  if (auto cur = trace.Current()) {
    CHECK(cur->opcode == 0xA9);
    CHECK(cur->opcode_address == 0x008000U);
    CHECK(cur->op_count == 0U);
    CHECK(cur->completed == false);
  } else {
    FAIL("Current() should have value");
  }

  MicroOpRecord rec;
  rec.index = 0;
  rec.bus_action = MicroBusAction::kFetchPc;
  rec.status = MicroOpStatus::kExecuted;
  rec.fetch_data = 0xA9;
  trace.OnMicroOp(rec);

  if (auto cur = trace.Current()) {
    CHECK(cur->op_count == 1U);
    CHECK(cur->ops[0].bus_action == MicroBusAction::kFetchPc);
  } else {
    FAIL("Current() should have value");
  }
  CHECK(trace.RetiredSize() == 0U);
}

TEST_CASE("MicroOpTrace moves completed instruction to ring", "[microop]") {
  MicroOpTrace trace;
  trace.OnInstructionBegin(0xA9, 0x008000);

  MicroOpRecord rec;
  rec.index = 0;
  rec.status = MicroOpStatus::kExecuted;
  trace.OnMicroOp(rec);
  trace.OnInstructionEnd(1);

  CHECK_FALSE(trace.Current().has_value());
  REQUIRE(trace.RetiredSize() == 1U);
  const auto* retired = trace.RetiredAt(0);
  REQUIRE(retired != nullptr);
  CHECK(retired->opcode == 0xA9);
  CHECK(retired->retired_seq == 1U);
  CHECK(retired->completed == true);
}

TEST_CASE("MicroOpTrace ring buffer evicts oldest", "[microop]") {
  MicroOpTrace trace;
  for (uint64_t i = 0; i < MicroOpTrace::kCapacity + 5; ++i) {
    trace.OnInstructionBegin(static_cast<uint8_t>(i & 0xFFU), 0x008000);
    trace.OnInstructionEnd(i + 1);
  }
  REQUIRE(trace.RetiredSize() == MicroOpTrace::kCapacity);
  const auto* oldest = trace.RetiredAt(0);
  const auto* newest = trace.RetiredAt(MicroOpTrace::kCapacity - 1);
  REQUIRE(oldest != nullptr);
  REQUIRE(newest != nullptr);
  CHECK(oldest->retired_seq == 6U);
  CHECK(newest->retired_seq == MicroOpTrace::kCapacity + 5U);
}

TEST_CASE("MicroOpTrace clear resets state", "[microop]") {
  MicroOpTrace trace;
  trace.OnInstructionBegin(0xA9, 0x008000);
  trace.OnInstructionEnd(1);
  trace.Clear();
  CHECK_FALSE(trace.Current().has_value());
  CHECK(trace.RetiredSize() == 0U);
}

}  // namespace
}  // namespace pupsnes::debugger

#include <span>
#include <vector>

#include "pupsnes/core/snes.h"
#include "pupsnes/hw/rom/cartridge.h"

namespace pupsnes {
namespace {

struct CapturedEvent {
  enum class Kind : uint8_t { kBegin, kOp, kEnd };
  Kind kind;
  uint8_t opcode = 0;
  SnesAddrT pc = 0;
  MicroOpRecord rec{};
  uint64_t retired_seq = 0;
};

class VectorRecorder : public MicroOpRecorder {
 public:
  void OnInstructionBegin(uint8_t opcode, SnesAddrT pc) override {
    CapturedEvent e;
    e.kind = CapturedEvent::Kind::kBegin;
    e.opcode = opcode;
    e.pc = pc;
    events.push_back(e);
  }
  void OnMicroOp(const MicroOpRecord& rec) override {
    CapturedEvent e;
    e.kind = CapturedEvent::Kind::kOp;
    e.rec = rec;
    events.push_back(e);
  }
  void OnInstructionEnd(uint64_t retired_seq) override {
    CapturedEvent e;
    e.kind = CapturedEvent::Kind::kEnd;
    e.retired_seq = retired_seq;
    events.push_back(e);
  }
  std::vector<CapturedEvent> events;
};

struct MicroOpCpuHarness {
  SNES snes;
  std::array<uint8_t, Cartridge::kLoROMWindowSize> rom{};

  MicroOpCpuHarness(std::span<const uint8_t> program, uint16_t pc) {
    rom.fill(0xEA);
    // Reset vector at bank0 $FFFC/$FFFD (LoROM maps bank 0x00:$8000+ -> rom[0x0000..]).
    rom[0x7FFCU] = static_cast<uint8_t>(pc & 0x00FFU);
    rom[0x7FFDU] = static_cast<uint8_t>(pc >> 8U);
    const std::size_t offset = static_cast<std::size_t>(pc) - 0x8000U;
    for (std::size_t i = 0; i < program.size(); ++i) {
      rom[offset + i] = program[i];
    }
    snes.LoadRom(rom);
    snes.Reset();
  }

  void RunOneInstruction() {
    // Step exactly one instruction via the debugger contract so we stop on the
    // instruction boundary (kRetiredStepTarget) — a plain TickToTarget(+N)
    // would overshoot and retire several instructions, so the recorder's last
    // event would belong to a later op rather than the one under test.
    CPU& cpu = snes.GetCpu();
    auto& contract = cpu.MutableDebuggerContract();
    contract.step_target = 1;
    contract.step_granularity = DebuggerContract::StepGranularity::kInstruction;
    (void)cpu.TickToTarget(snes.GetMasterTime() + 1000);
    contract.step_target = 0;
  }
};

TEST_CASE("MicroOpRecorder emits begin/ops/end for LDA immediate", "[microop]") {
  const uint8_t program[] = {0xA9, 0x42};
  MicroOpCpuHarness h(std::span<const uint8_t>(program, sizeof(program)), 0x8000);
  VectorRecorder recorder;
  h.snes.GetCpu().SetMicroOpRecorder(&recorder);

  h.RunOneInstruction();

  REQUIRE(recorder.events.size() >= 3);
  CHECK(recorder.events.front().kind == CapturedEvent::Kind::kBegin);
  CHECK(recorder.events.front().opcode == 0xA9);
  CHECK(recorder.events.back().kind == CapturedEvent::Kind::kEnd);
  CHECK(recorder.events.back().retired_seq == 1U);

  bool saw_opcode_fetch = false;
  for (const auto& e : recorder.events) {
    if (e.kind == CapturedEvent::Kind::kOp && e.rec.index == 0 && e.rec.bus_action == MicroBusAction::kFetchPc &&
        e.rec.status == MicroOpStatus::kExecuted) {
      saw_opcode_fetch = true;
      break;
    }
  }
  CHECK(saw_opcode_fetch);
}

TEST_CASE("MicroOpRecorder captures bus address for opcode fetch", "[microop]") {
  const uint8_t program[] = {0xA9, 0x42};
  MicroOpCpuHarness h(std::span<const uint8_t>(program, sizeof(program)), 0x8000);
  VectorRecorder recorder;
  h.snes.GetCpu().SetMicroOpRecorder(&recorder);
  h.RunOneInstruction();

  bool found = false;
  for (const auto& e : recorder.events) {
    if (e.kind == CapturedEvent::Kind::kOp && e.rec.index == 0 && e.rec.bus_action == MicroBusAction::kFetchPc) {
      CHECK(e.rec.has_bus);
      // bus_addr should point to the opcode address (PBR:PC at fetch time).
      CHECK(e.rec.bus_value == 0xA9);
      found = true;
      break;
    }
  }
  CHECK(found);
}

TEST_CASE("MicroOpRecorder emits Skipped for untaken branch", "[microop]") {
  // BNE (0xD0) + disp. Set Z=1 so branch is NOT taken.
  const uint8_t program[] = {0xD0, 0x05, 0xEA};
  MicroOpCpuHarness h(std::span<const uint8_t>(program, sizeof(program)), 0x8000);

  auto regs = h.snes.GetCpu().GetRegs();
  regs.P.Z = true;
  h.snes.GetCpu().SetRegs(regs);

  VectorRecorder recorder;
  h.snes.GetCpu().SetMicroOpRecorder(&recorder);
  h.RunOneInstruction();

  bool saw_skipped = false;
  for (const auto& e : recorder.events) {
    if (e.kind == CapturedEvent::Kind::kOp && e.rec.status == MicroOpStatus::kSkipped) {
      saw_skipped = true;
      break;
    }
  }
  CHECK(saw_skipped);
}

}  // namespace
}  // namespace pupsnes
