// End-to-end NMI / VSYNC plumbing tests. Cover:
//   * PPU /NMI line level tracking V-threshold transitions with overscan.
//   * CPU flip-flop latches on falling edge through NMITIMEN gate.
//   * NMITIMEN 0→1 transparency quirk (immediate NMI) and 1→0 cancellation
//     quirk (clears the pending flip-flop).
//   * Synthetic HW interrupt entry dispatch: PC jumps to vector, handler
//     runs, flags pushed with B=0 in emulation.
//   * WAI wakes on NMI assertion, takes the 2-cycle internal wake latency,
//     then delivers. STP never wakes.
//
// Tests drive the machine via TickToTarget through a real LoROM ResetFixture
// (the same one the CPU tests use). The reset ROM traps the CPU in a tight
// BRA $-2 loop at the entry point so VBlank can fire while the CPU is making
// forward progress, and the NMI vector ($00FFFA / $00FFEA) is populated via
// SetRomByte at the corresponding LoROM offset.
//
// Note: the LoROM window starts at $8000 of bank $00. The reset vector slot
// at $FFFC-$FFFD is offset 0x7FFC inside the 32 KB ROM window. The NMI
// vectors live at $FFFA (emulation) and $FFEA (native), offset 0x7FFA /
// 0x7FEA respectively.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <initializer_list>

#include "cpu_test_fixture.h"
#include "pupsnes/hw/5a22/cpu.h"
#include "pupsnes/hw/5a22/cpu_mmio.h"
#include "pupsnes/hw/rom/cartridge.h"
#include "pupsnes/core/scheduler.h"
#include "pupsnes/core/snes.h"
#include "pupsnes/hw/sppu/ppu.h"
#include "pupsnes/hw/sppu/ppu_regs.h"
#include "pupsnes/memory/systembus.h"

using namespace pupsnes;  // NOLINT(google-build-using-namespace)
using pupsnes::test::ResetFixture;

namespace {

// Master cycle at which V steps onto the first VBlank line (no overscan).
constexpr TimeMasterT kStartOfV225 = 225U * 1364U;
constexpr TimeMasterT kStartOfV240 = 240U * 1364U;

// Installs NMI and reset vectors, plus a BRA $-2 spin at the reset entry so
// the CPU has something to do while the PPU walks toward VBlank.
void SetupSpinRomWithNmiVector(ResetFixture& f, uint16_t reset_entry, uint16_t nmi_emu_vector,
                               uint16_t nmi_native_vector) {
  f.SetResetVector(reset_entry);
  // Emulation NMI vector at $FFFA.
  f.rom[0x7FFAU] = static_cast<uint8_t>(nmi_emu_vector & 0xFFU);
  f.rom[0x7FFBU] = static_cast<uint8_t>(nmi_emu_vector >> 8U);
  // Native NMI vector at $FFEA.
  f.rom[0x7FEAU] = static_cast<uint8_t>(nmi_native_vector & 0xFFU);
  f.rom[0x7FEBU] = static_cast<uint8_t>(nmi_native_vector >> 8U);
  // BRA $-2 at reset entry. Offset inside the 32KB LoROM image = entry - $8000.
  const std::size_t entry_off = static_cast<std::size_t>(reset_entry - 0x8000U);
  f.rom[entry_off + 0] = 0x80U;  // BRA
  f.rom[entry_off + 1] = 0xFEU;  // -2 (loop forever)
  f.SyncCartridge();
}

void EnableNmi(ResetFixture& f, TimeMasterT t) {
  BusPlan plan = f.snes.system_bus->Plan(0x00'4200U, BusAccessType::kWrite, 0x80U);
  REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
  (void)f.snes.system_bus->Follow(plan, t, 0x80U);
}

void DisableNmi(ResetFixture& f, TimeMasterT t) {
  BusPlan plan = f.snes.system_bus->Plan(0x00'4200U, BusAccessType::kWrite, 0x00U);
  REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
  (void)f.snes.system_bus->Follow(plan, t, 0x00U);
}

// Drive the machine until master time reaches `target`, chunked by scheduler
// events so the VBlank-NMI boundary fence can fire at the correct master
// cycle. Real-machine equivalent of "run the machine for N master cycles."
// Without this, CPU::TickToTarget would race past scheduler events and miss
// the sync fence — time travel is forbidden per the design (see
// project_design_decisions.md).
void RunTo(SNES& snes, TimeMasterT target) {
  while (snes.GetMasterTime() < target) {
    Scheduler& sched = snes.GetScheduler();
    TimeMasterT next_chunk = target;
    if (sched.HasPendingEvents()) {
      next_chunk = std::min(next_chunk, sched.NextEventMasterTime());
    }
    if (snes.GetMasterTime() < next_chunk) {
      (void)snes.GetCpu().TickToTarget(next_chunk);
    }
    snes.MachineSync(snes.GetMasterTime());
    sched.FireEventsThrough(snes.GetMasterTime());
  }
}

}  // namespace

// ---------- PPU /NMI line level ----------

TEST_CASE("Ppu::SampleNmiLine tracks V=225 falling/rising edges (no overscan)", "[unit][ppu]") {
  SNES snes;
  snes.Reset();
  Ppu& ppu = snes.GetPpu();

  // Before V=225: line deasserted.
  REQUIRE_FALSE(ppu.SampleNmiLine(0));
  REQUIRE_FALSE(ppu.SampleNmiLine(224U * 1364U));

  // On V=225: line asserted for the duration of that scanline.
  REQUIRE(ppu.SampleNmiLine(kStartOfV225));
  REQUIRE(ppu.SampleNmiLine(kStartOfV225 + 1000));

  // After V=226: line deasserted again.
  REQUIRE_FALSE(ppu.SampleNmiLine(226U * 1364U));
}

TEST_CASE("Ppu::SampleNmiLine tracks V=240 threshold under SETINI overscan", "[unit][ppu]") {
  SNES snes;
  snes.Reset();
  Ppu& ppu = snes.GetPpu();

  // Enable overscan at t=0 (write to SETINI $2133).
  BusPlan plan = snes.system_bus->Plan(0x00'2133U, BusAccessType::kWrite, sppu::regs::kSetiniOverscanMask);
  REQUIRE(plan.outcome == BusPlanOutcome::kInlineComplete);
  (void)snes.system_bus->Follow(plan, 0, sppu::regs::kSetiniOverscanMask);

  // At V=225 overscan shifts VBlank entry to V=240, so line stays deasserted.
  REQUIRE_FALSE(ppu.SampleNmiLine(kStartOfV225));

  // At V=240 the line asserts.
  REQUIRE(ppu.SampleNmiLine(kStartOfV240));
}

// ---------- CPU flip-flop + NMITIMEN gate ----------

TEST_CASE("NMI is not delivered while NMITIMEN.7 is clear", "[unit][cpu][nmi]") {
  ResetFixture f;
  SetupSpinRomWithNmiVector(f, /*reset_entry=*/0x8000U,
                            /*nmi_emu_vector=*/0x9000U, /*nmi_native_vector=*/0x9100U);
  f.snes.Reset();

  // Do NOT enable NMITIMEN. Run past VBlank and confirm PC is still in the
  // reset loop (PC in [$8000, $8002]).
  RunTo(f.snes, kStartOfV225 + 200U);

  const auto regs = f.cpu.GetRegs();
  REQUIRE(regs.PC >= 0x8000U);
  REQUIRE(regs.PC <= 0x8002U);
  REQUIRE(regs.PBR == 0U);
}

TEST_CASE("NMI is delivered when NMITIMEN.7 is set and VBlank arrives", "[unit][cpu][nmi]") {
  ResetFixture f;
  SetupSpinRomWithNmiVector(f, /*reset_entry=*/0x8000U,
                            /*nmi_emu_vector=*/0x9000U, /*nmi_native_vector=*/0x9100U);
  f.snes.Reset();

  // Enable NMI early so the falling edge at V=225 latches the flip-flop.
  EnableNmi(f, /*t=*/0);
  // Run well past the first VBlank so the pending NMI has been delivered and
  // the handler's first few cycles have executed.
  RunTo(f.snes, kStartOfV225 + 2000U);

  const auto regs = f.cpu.GetRegs();
  // On emulation-mode reset, PC jumped to the emulation NMI vector $9000.
  REQUIRE(regs.PBR == 0U);
  // Handler may have executed one or more NOPs (0xEA fill); PC >= $9000 and
  // still close to the handler entry.
  // Handler ran into 0xEA (NOP) fill and drifts forward. Just confirm we're
  // past the handler entry and still in handler-land (not back in reset spin).
  REQUIRE(regs.PC >= 0x9000U);
  REQUIRE(regs.PC < 0x9400U);
  // NMI path sets I=1 on handler entry.
  REQUIRE(regs.P.I == true);
  // NMI path clears D.
  REQUIRE(regs.P.D == false);
}

// ---------- NMITIMEN quirks ----------

TEST_CASE("NMITIMEN 0→1 transparency fires immediate NMI while in VBlank", "[unit][cpu][nmi]") {
  ResetFixture f;
  SetupSpinRomWithNmiVector(f, /*reset_entry=*/0x8000U,
                            /*nmi_emu_vector=*/0x9000U, /*nmi_native_vector=*/0x9100U);
  f.snes.Reset();

  // Run the CPU into VBlank with NMITIMEN disabled. The PPU has /NMI asserted
  // on its pin, but the CPU's flip-flop stays clear (gate closed).
  RunTo(f.snes, kStartOfV225 + 100U);
  REQUIRE(f.cpu.GetRegs().PC >= 0x8000U);
  REQUIRE(f.cpu.GetRegs().PC <= 0x8002U);

  // Flip NMITIMEN 0→1. The AND-gate output rises immediately, triggering the
  // flip-flop. On the very next instruction boundary, NMI delivers.
  EnableNmi(f, kStartOfV225 + 100U);
  RunTo(f.snes, kStartOfV225 + 2000U);

  const auto regs = f.cpu.GetRegs();
  // Handler ran into 0xEA (NOP) fill and drifts forward. Just confirm we're
  // past the handler entry and still in handler-land (not back in reset spin).
  REQUIRE(regs.PC >= 0x9000U);
  REQUIRE(regs.PC < 0x9400U);
}

TEST_CASE("NMITIMEN 1→0 cancels a pending (not-yet-delivered) NMI", "[unit][cpu][nmi]") {
  // Arm the flip-flop by entering VBlank with NMI enabled, but do not give
  // the CPU time to run the dispatch — i.e., don't tick past VBlank entry.
  // Then clear NMITIMEN before any further Ticks. The pending flip-flop
  // must be cleared, and no handler dispatch occurs.
  ResetFixture f;
  SetupSpinRomWithNmiVector(f, /*reset_entry=*/0x8000U,
                            /*nmi_emu_vector=*/0x9000U, /*nmi_native_vector=*/0x9100U);
  f.snes.Reset();

  EnableNmi(f, /*t=*/0);

  // Advance master time to VBlank entry. The bus write below will catch the
  // PPU up and see the line asserted. But the CPU hasn't yet sampled — so
  // cancellation by disabling NMITIMEN must discard the would-be NMI.
  //
  // We can't avoid the CPU Tick entirely because scheduler fences force
  // chunking; we simulate "NMI pending but not delivered" by disabling the
  // gate before the first post-VBlank instruction boundary.
  //
  // Trick: write to $4200 at kStartOfV225 before the CPU has ticked to that
  // point. The OnNmiTimenChanged handler samples the line at t, sees it
  // asserted (if gate was going 0→1) or deasserted for the cancellation
  // path. Since we start with gate on, we flip it 1→0 at t=kStartOfV225, and
  // the handler clears any pending.
  DisableNmi(f, kStartOfV225);

  // Now tick well past VBlank. No handler dispatch should happen.
  RunTo(f.snes, kStartOfV225 + 2000U);

  const auto regs = f.cpu.GetRegs();
  REQUIRE(regs.PC >= 0x8000U);
  REQUIRE(regs.PC <= 0x8002U);
}

// ---------- RDNMI independence from NMITIMEN ----------

TEST_CASE("RDNMI ($4210) latch arms independently of NMITIMEN.7", "[unit][cpu_mmio][nmi]") {
  SNES snes;
  snes.Reset();
  // NMITIMEN stays 0. The $4210 latch still arms at VBlank entry because it's
  // fed by the raw PPU /NMI line, not through the NMITIMEN gate.
  BusPlan plan = snes.system_bus->Plan(0x00'4210U, BusAccessType::kRead);
  auto result = snes.system_bus->Follow(plan, kStartOfV225, 0);
  REQUIRE((result.data & CpuMmio::kRdNmiVblankFlagMask) != 0U);
}

// ---------- Synthetic HW interrupt entry: P push with B=0 ----------

TEST_CASE("HW interrupt entry pushes P with B=0 in emulation mode", "[unit][cpu][nmi]") {
  // Set a handler that reads the pushed P off the stack into A. We don't want
  // RTI; a simple BRA $-2 after PLA keeps the CPU spinning at a deterministic
  // location where we can inspect state.
  //
  // Handler at $9000:
  //   PLA  (pull P into A low byte)
  //   $-2 spin
  ResetFixture f;
  SetupSpinRomWithNmiVector(f, /*reset_entry=*/0x8000U,
                            /*nmi_emu_vector=*/0x9000U, /*nmi_native_vector=*/0x9100U);
  f.rom[0x1000U] = 0x68U;  // PLA  (at $9000)
  f.rom[0x1001U] = 0x80U;  // BRA
  f.rom[0x1002U] = 0xFEU;  // -2
  f.SyncCartridge();
  f.snes.Reset();

  EnableNmi(f, 0);
  RunTo(f.snes, kStartOfV225 + 3000U);

  // The handler pulled the pushed P byte into A. In emulation mode, the HW
  // interrupt push clears B (bit 4). Verify A's low 8 bits have bit 4 clear.
  const auto regs = f.cpu.GetRegs();
  REQUIRE((regs.A & 0x10U) == 0U);
}

// ---------- WAI wake ----------

TEST_CASE("WAI halts until NMI assertion, then delivers", "[unit][cpu][nmi][wai]") {
  // Program at reset: WAI, NOP, NOP, ..., BRA -2.
  ResetFixture f;
  SetupSpinRomWithNmiVector(f, /*reset_entry=*/0x8000U,
                            /*nmi_emu_vector=*/0x9000U, /*nmi_native_vector=*/0x9100U);
  f.rom[0x0000U] = 0xCBU;  // WAI
  f.rom[0x0001U] = 0xEAU;  // NOP (would be next instruction after wake)
  f.rom[0x0002U] = 0x80U;  // BRA
  f.rom[0x0003U] = 0xFDU;  // -3 (loop back to NOP)
  f.SyncCartridge();
  f.snes.Reset();

  EnableNmi(f, 0);

  // Let WAI execute (3 cycles), then the CPU halts. Running past VBlank
  // should wake it and land in the NMI handler.
  RunTo(f.snes, kStartOfV225 + 3000U);

  const auto regs = f.cpu.GetRegs();
  // Handler ran into 0xEA (NOP) fill and drifts forward. Just confirm we're
  // past the handler entry and still in handler-land (not back in reset spin).
  REQUIRE(regs.PC >= 0x9000U);
  REQUIRE(regs.PC < 0x9400U);
}

TEST_CASE("WAI wake latency is independent of target slice size", "[unit][cpu][nmi][wai]") {
  // Keep NMITIMEN clear: the raw NMI pin wakes WAI, but no interrupt entry
  // obscures the exact twelve-master-cycle wake duration.
  for (const TimeMasterDeltaT slice : {1U, 2U, 3U, 4U, 5U, 6U, 7U, 11U, 12U}) {
    CAPTURE(slice);
    ResetFixture f;
    f.LoadInstruction({0xCB, 0xEA});
    f.snes.Reset();
    f.cpu.MutableDebuggerContract().step_target = 1;
    REQUIRE(f.cpu.TickToTarget(100).reason == TickStopReason::kRetiredStepTarget);
    REQUIRE(f.cpu.GetHaltState() == HaltState::kWai);
    REQUIRE(f.cpu.GetRetiredInstructionCount() == 1U);

    // The halted CPU advances to VBlank without executing another opcode.
    REQUIRE(f.cpu.TickToTarget(kStartOfV225).reason == TickStopReason::kReachedTarget);
    for (TimeMasterDeltaT elapsed = 0; elapsed < 12U;) {
      const TimeMasterDeltaT step = std::min<TimeMasterDeltaT>(slice, 12U - elapsed);
      elapsed += step;
      const TickResult result = f.cpu.TickToTarget(kStartOfV225 + elapsed);
      CAPTURE(elapsed);
      REQUIRE(result.completed_cycles == step);
      REQUIRE(f.snes.GetMasterTime() == kStartOfV225 + elapsed);
      REQUIRE(f.cpu.GetWaiWakeCyclesRemaining() == (12U - elapsed + 5U) / 6U);
      REQUIRE(f.cpu.GetHaltState() == (elapsed < 12U ? HaltState::kWai : HaltState::kNone));
      REQUIRE(f.cpu.GetRegs().PC == 0x8001U);
      REQUIRE(f.cpu.GetRetiredInstructionCount() == 1U);
    }

    f.cpu.MutableDebuggerContract().step_target = 1;
    const TickResult resumed = f.cpu.TickToTarget(kStartOfV225 + 100U);
    REQUIRE(resumed.reason == TickStopReason::kRetiredStepTarget);
    REQUIRE(resumed.completed_cycles == 14U);  // NOP: one 8-cycle fetch + 6 internal.
    REQUIRE(f.cpu.GetRegs().PC == 0x8002U);
  }
}

TEST_CASE("Reset discards a partially served WAI wake cycle", "[unit][cpu][nmi][wai]") {
  ResetFixture f;
  f.LoadInstruction({0xCB, 0xEA});
  for (unsigned attempt = 0; attempt < 2U; ++attempt) {
    f.snes.Reset();
    f.cpu.MutableDebuggerContract().step_target = 1;
    REQUIRE(f.cpu.TickToTarget(100).reason == TickStopReason::kRetiredStepTarget);
    (void)f.cpu.TickToTarget(kStartOfV225);
    // Reset the first run after five wake cycles. The second run must still
    // need all twelve cycles rather than inherit those five banked cycles.
    const TimeMasterDeltaT elapsed = attempt == 0U ? 5U : 11U;
    (void)f.cpu.TickToTarget(kStartOfV225 + elapsed);
    REQUIRE(f.cpu.GetHaltState() == HaltState::kWai);
    if (attempt == 1U) {
      REQUIRE(f.cpu.GetWaiWakeCyclesRemaining() == 1U);
      (void)f.cpu.TickToTarget(kStartOfV225 + 12U);
      REQUIRE(f.cpu.GetHaltState() == HaltState::kNone);
    }
  }
}

TEST_CASE("STP halts permanently — NMI does not wake", "[unit][cpu][nmi][stp]") {
  ResetFixture f;
  SetupSpinRomWithNmiVector(f, /*reset_entry=*/0x8000U,
                            /*nmi_emu_vector=*/0x9000U, /*nmi_native_vector=*/0x9100U);
  f.rom[0x0000U] = 0xDBU;  // STP
  f.rom[0x0001U] = 0xEAU;  // NOP (never executed)
  f.SyncCartridge();
  f.snes.Reset();

  EnableNmi(f, 0);
  RunTo(f.snes, kStartOfV225 + 3000U);

  // STP halted the CPU before NMITIMEN took effect on an instruction boundary
  // that could have delivered. Even after VBlank, the CPU must remain stopped
  // (PC still at the STP opcode fetch address or just past its 3-cycle entry).
  const auto regs = f.cpu.GetRegs();
  // Implementation detail: STP advances PC past the opcode + the 2 internal
  // halt-entry cycles don't change PC further. PC is at $8001 when STP
  // completes its fetch and cycle 1 runs. We just need to confirm we didn't
  // land in the NMI handler.
  REQUIRE(regs.PC < 0x9000U);
}

// ---------- Re-arm across frames ----------

TEST_CASE("NMI re-arms for the next frame after handler acknowledgment", "[unit][cpu][nmi]") {
  // Handler at $9000: SEI + CLI + BRA -2. The flip-flop is cleared on entry;
  // re-entering VBlank on the next frame must set it again.
  //
  // A simpler test: just run for two frames and check that RDNMI latches
  // high once per frame. (NMI delivery itself is covered by earlier tests.)
  SNES snes;
  snes.Reset();
  BusPlan enable = snes.system_bus->Plan(0x00'4200U, BusAccessType::kWrite, 0x80U);
  (void)snes.system_bus->Follow(enable, 0, 0x80U);

  // Clear the first-frame latch by reading RDNMI inside VBlank.
  BusPlan r1 = snes.system_bus->Plan(0x00'4210U, BusAccessType::kRead);
  auto first = snes.system_bus->Follow(r1, kStartOfV225, 0);
  REQUIRE((first.data & CpuMmio::kRdNmiVblankFlagMask) != 0U);

  // Second frame's VBlank: latch should arm again.
  constexpr TimeMasterT kSecondFrameVblank = (262U * 1364U) + (225U * 1364U);
  BusPlan r2 = snes.system_bus->Plan(0x00'4210U, BusAccessType::kRead);
  auto second = snes.system_bus->Follow(r2, kSecondFrameVblank, 0);
  REQUIRE((second.data & CpuMmio::kRdNmiVblankFlagMask) != 0U);
}
