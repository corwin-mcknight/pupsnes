#pragma once

#include <cstdint>
#include <optional>

#include "pupsnes/hw/5a22/cpu_regs.h"
#include "pupsnes/hw/debugger_contract.h"
#include "pupsnes/hw/master_clock_driver.h"
#include "pupsnes/hw/systembus.h"
#include "pupsnes/types.h"

namespace pupsnes {

// SNES timing constants for CPU-owned DRAM refresh.
//
// The 5A22 stalls the bus for 40 master cycles every scanline to refresh DRAM.
// The pause begins at roughly master-cycle offset 538 into each 1364-cycle
// scanline. We model this as a CPU-internal stall: no bus ops are issued
// during the window, and local_time advances normally.
inline constexpr TimeMasterDeltaT kMasterCyclesPerScanline = 1364;
inline constexpr TimeMasterDeltaT kDramRefreshStartCycle = 538;
inline constexpr TimeMasterDeltaT kDramRefreshDurationCycles = 40;

// Master-cycle cost of a CPU-internal (non-bus) cycle. Bus cycles charge
// access_speed from the page table (6 for FASTROM, 8 for slow ROM/WRAM/MMIO,
// 12 for joypad). Internal cycles always run at the CPU's intrinsic 6-cycle
// pace — the MEMSEL speed only affects bus transactions.
inline constexpr TimeMasterDeltaT kInternalCpuCycleMaster = 6;

class SNES;

// Forward declarations for micro-op observation types. Full definitions live
// in pupsnes/hw/5a22/micro_op.h; consumers that need to construct or unpack
// MicroOpRecord values include that header directly. cpu.h only references
// these types by pointer or in non-inline function signatures, so a forward
// declaration is sufficient.
enum class MicroBusAction : uint8_t;
enum class MicroInternalOp : uint8_t;
struct MicroOpRecord;
class MicroOpRecorder;


// Typed enums for MicroOp::params packing. Populated in subsequent refactor
// steps as each enum group collapses into a parameterized category. For now
// they exist alongside MicroInternalOp; no opcode uses them yet.
enum class Reg : uint8_t { kA, kX, kY, kSp, kDp, kDbr, kP };
enum class Width : uint8_t { kByMFlag, kByXFlag, kForce8, kForce16 };
enum class ByteSel : uint8_t { kLow, kHigh, kBank };
// Bank byte source for kSetAddrByteFromFetch when writing the high byte in the
// same cycle. Packed alongside ByteSel in the micro-op params (bits [3:2]).
// kLeave preserves addr_[23:16] unchanged (used when the bank will be fetched
// explicitly on a later cycle). kDbr/kPbr/kZero set it from DBR/PBR/0
// respectively — see micro_op_params::PackSetAddrByte.
enum class BankSrc : uint8_t { kLeave, kDbr, kPbr, kZero };
enum class Flag : uint8_t { kC, kD, kI, kV, kZ, kN, kM, kX };
enum class BranchCond : uint8_t { kAlways, kZ, kNotZ, kC, kNotC, kN, kNotN, kV, kNotV };
// kBitMem: memory BIT — sets N from bit 7/15 and V from bit 6/14 of the memory
// operand, Z from (A & operand). Distinct from kBit (immediate BIT, Z-only).
// Ripple audit (2026-04-21): PackAluOp packs into bits [3:0] (4 bits, capacity 16);
// kBitMem = 9 fits. The only exhaustive AluOp switch is in cpu.cpp's
// kAlu8Imm/kAlu16Imm dispatch (updated to handle kBitMem).
enum class AluOp : uint8_t { kAdc, kSbc, kAnd, kOra, kEor, kCmp, kCpx, kCpy, kBit, kBitMem };
enum class ShiftOp : uint8_t { kAsl, kLsr, kRol, kRor };
// Memory read-modify-write ops. kAsl/kLsr/kRol/kRor mirror ShiftOp; kInc/kDec
// are the memory forms of INC/DEC. kTsb/kTrb are the test-and-set / test-and-
// reset bit ops (Bruce Clark §6.1.2.3): result = mem | A or mem & ~A, and the
// only flag updated is Z, computed from (A & mem). Packed into a
// MicroInternalOp::kRmwMem params byte (see micro_op_params::PackRmw); the
// three-bit field has room for eight variants.
enum class RmwOp : uint8_t { kAsl, kLsr, kRol, kRor, kInc, kDec, kTsb, kTrb };
// kScratchLow: write the low byte of addr_scratch_ (used by 16-bit RMW to
// emit the modified low byte back to memory on the final write cycle).
enum class WriteSrc : uint8_t { kFetchData, kA, kX, kY, kZero, kScratchLow };
enum class PushSrc : uint8_t {
  kA8,
  kAHigh,
  kX8,
  kXHigh,
  kY8,
  kYHigh,
  kPcl,
  kPch,
  kPbr,
  kDbr,
  kP,       // Push P as-is. Used by PHP and by SW interrupt entry (BRK/COP):
            // P.ToByte() sets B=1 in E=1, matching BRK semantics.
  kPHwIrq,  // Push P with the B bit cleared in E=1 (distinguishes HW interrupts
            // from BRK to the handler). In native (E=0) the bit position carries
            // the X flag and is pushed as-is, same as kP.
  kDpLow,
  kDpHigh,
  kAddrLow,
  kAddrHigh,
};

// Hardware / software interrupt kinds for MicroInternalOp::kSetInterruptVector.
// Values are stable across rebuilds — treat as part of the save-state format
// once the state block migration lands.
enum class InterruptKind : uint8_t {
  kBrk = 0,
  kCop = 1,
  kNmi = 2,
  kIrq = 3,
  kAbort = 4,
};

// CPU halt state. kNone is the default (running normally). kEmulation is a
// reserved slot for debugger / emulator-induced halts (not currently wired;
// run-control suspends via DebuggerContract instead). kWai is set by WAI,
// cleared by any interrupt assertion (regardless of I flag) after a 2-cycle
// internal wake latency. kStp is set by STP, cleared only by Reset().
enum class HaltState : uint8_t {
  kNone = 0,
  kEmulation = 1,
  kWai = 2,
  kStp = 3,
};

// Forward declaration only — the InstructionEntry layout lives in
// src/pupsnes/5a22/cpu_internal.h. CPU::current_instr_ holds a pointer to
// entries in the opcode table defined alongside cpu.cpp; no caller of cpu.h
// needs the complete type.
struct InstructionEntry;

// 65C816 CPU device.
//
// tick() walks a micro-op table: cycle 0 always fetches the opcode via the
// SystemBus, then the per-opcode remaining ops execute one per cycle. All bus
// accesses use SystemBus plan/follow.
class CPU : public MasterClockDriver {
 public:
  // Alias so existing call sites can keep using CPU::Regs while the type lives
  // in a standalone header for reuse without pulling in the full CPU class.
  using Regs = CpuRegs;

  struct Fault {
    enum class Type : uint8_t {
      kUnimplementedOpcode = 0,
    };

    Type type = Type::kUnimplementedOpcode;
    uint8_t opcode = 0;
    SnesAddrT opcode_address = 0;
    Regs regs{};
  };

  explicit CPU(SNES* snes);
  ~CPU() override = default;

  [[nodiscard]] const char* DeviceName() const override { return "CPU"; }

  void Reset();

  [[nodiscard]] TickResult TickToTarget(TimeMasterT target_master_time) override;

  [[nodiscard]] Regs GetRegs() const { return regs_; }
  void SetRegs(const Regs& r) { regs_ = r; }
  [[nodiscard]] uint8_t GetMicroOpIndex() const { return micro_op_index_; }
  [[nodiscard]] const std::optional<Fault>& GetFault() const { return fault_; }

  // Test-only: inject a fault as if the CPU encountered an unimplemented
  // opcode at `address`. Exists so debugger/run-control tests can exercise
  // the fault-handling path without relying on an unimplemented opcode
  // (every 65C816 opcode is now implemented). Not intended for production
  // callers.
  void DebugInjectFault(uint8_t opcode, SnesAddrT address);
  [[nodiscard]] uint64_t GetRetiredInstructionCount() const { return retired_instruction_count_; }

  // Test / debugger visibility into interrupt state.
  [[nodiscard]] HaltState GetHaltState() const { return halt_state_; }
  [[nodiscard]] bool IsNmiPending() const { return nmi_pending_; }
  [[nodiscard]] uint8_t GetWaiWakeCyclesRemaining() const { return wai_wake_cycles_remaining_; }

  // DRAM-refresh telemetry. Windows counts the number of 40-cycle refresh
  // pauses the CPU has served since reset; cycles is the cumulative stall
  // total (normally windows * kDramRefreshDurationCycles unless a window was
  // cut short by a budget boundary, in which case the remainder is counted
  // on the next Tick). next-time is the absolute master time at which the
  // next refresh window will begin.
  [[nodiscard]] uint64_t GetRefreshStallWindows() const { return retired_refresh_windows_; }
  [[nodiscard]] uint64_t GetRefreshStallCycles() const { return retired_refresh_cycles_; }
  [[nodiscard]] TimeMasterT GetNextRefreshTime() const { return next_refresh_time_; }

  void SetMicroOpRecorder(MicroOpRecorder* recorder) { micro_op_recorder_ = recorder; }
  [[nodiscard]] MicroOpRecorder* GetMicroOpRecorder() const { return micro_op_recorder_; }

  // The debugger (RunControl) owns the contract and mutates it between Tick
  // calls to request step-N behavior or acknowledge a suppressed breakpoint.
  // CPU reads it inside FetchOpcode / on instruction retire and writes back
  // into suppressed_breakpoint_pc / step_target as it hits stop conditions.
  void SetDebuggerContract(const DebuggerContract& contract) { debugger_contract_ = contract; }
  [[nodiscard]] const DebuggerContract& GetDebuggerContract() const { return debugger_contract_; }
  [[nodiscard]] DebuggerContract& MutableDebuggerContract() { return debugger_contract_; }

 private:
  struct TimingContext {
    bool branch_taken = false;
    bool branch_page_crossed = false;
    // Cached at opcode-fetch from (regs_.DP & 0xFF) != 0. Folded into the
    // same rule-table slot as branch_page_crossed via an OR in
    // EvaluateTimingRule; see the kDirectPageLowNonzero alias in
    // cpu_internal.h.
    bool dp_low_nonzero = false;
  };

  struct StepResult {
    // Master-cycle cost of the micro-op that just retired. 0 means no
    // micro-op was consumed (breakpoint / fault-before-retire); the caller
    // checks master_cycles > 0 to decide whether to advance debugger state.
    // `stopped` is true when the step hit a breakpoint or fault; the caller
    // maps `reason` to kBreakpoint or kFault. kReachedTarget (stopped=false)
    // means normal progress — the loop continues.
    TimeMasterDeltaT master_cycles = 0;
    TickStopReason reason = TickStopReason::kReachedTarget;
    bool stopped = false;
  };

  Regs regs_;

  // Micro-op execution state.
  // micro_op_index_ == 0 means we are between instructions (next cycle fetches
  // the opcode); 1..N indexes into the current instruction's remaining ops.
  uint8_t micro_op_index_ = 0;
  uint8_t fetch_data_ = 0;  // Last byte read from bus
  uint32_t addr_ = 0;       // Effective address accumulator
  // Indirect-pointer scratch for multi-byte effective-address assembly.
  // Models the 65C816's internal AAL/AAH latches: while addr_ points at the
  // pointer being read, each pointer byte is stashed here so it survives the
  // next bus-read that clobbers fetch_data_. Used by (dp), [dp], (dp,X),
  // (dp),Y, [dp],Y, JMP (abs), JMP [abs], JMP (abs,X), JSR (abs,X).
  uint16_t addr_scratch_ = 0;
  TimingContext timing_context_{};
  std::optional<Fault> fault_ = std::nullopt;
  // CPU halt state (see HaltState enum). kStp is cleared only by Reset().
  // kWai is cleared when any interrupt pin asserts, after a 2-cycle internal
  // wake latency tracked in wai_wake_cycles_remaining_.
  HaltState halt_state_ = HaltState::kNone;
  // Countdown of internal cycles remaining in the WAI wake sequence. Set to 2
  // when a wake signal is first observed while kWai; each subsequent
  // TickToTarget step charges kInternalCpuCycleMaster and decrements. When
  // this reaches 0 halt_state_ transitions to kNone and normal dispatch
  // resumes (the next instruction-boundary sample delivers the interrupt if
  // still gated appropriately).
  uint8_t wai_wake_cycles_remaining_ = 0;

  // --- Interrupt state ---
  // Raw /NMI line level from the PPU, sampled at instruction boundaries.
  // This is the pin level AFTER the NMITIMEN.7 AND-gate — i.e., the value
  // whose falling edges set nmi_pending_. nmi_gated_prev_ stores the
  // previous sample of the same AND-gate output so edge detection compares
  // current vs previous. Note: the PPU's /NMI line level is exposed via
  // Ppu::SampleNmiLine (catches PPU up) / Ppu::PeekNmiLine (no catch-up).
  bool nmi_curr_ = false;
  bool nmi_gated_prev_ = false;
  // Latched NMI request flip-flop. Set on the falling edge of the gated line
  // (or by the NMITIMEN 0→1 transparency quirk when the line is already low).
  // Cleared when the hardware-interrupt entry dispatches, or by NMITIMEN.7
  // being written 0 (the gate going low clears the flip-flop).
  bool nmi_pending_ = false;

  // Placeholders for future ABORT / IRQ wiring. No device drives these today;
  // fields exist so the state-block layout is stable when those signals land.
  bool abort_pending_ = false;
  bool irq_line_asserted_ = false;
  uint64_t retired_instruction_count_ = 0;
  MicroOpRecorder* micro_op_recorder_ = nullptr;
  DebuggerContract debugger_contract_{};
  // Trace entry captured at instruction-begin (PC + pre-execute regs) and
  // pushed to the trace sink when the instruction retires.
  TraceEntry pending_trace_{};
  const InstructionEntry* current_instr_ = nullptr;
  // Cached at FetchOpcode time from current_instr_->rule_count > 1. Lets the
  // per-micro-op drain guard be a single-load/branch check instead of
  // dereferencing current_instr_. Cleared in FinishInstruction / RecordFault.
  bool needs_drain_ = false;
  // Cached at Reset() from snes_->system_bus.get(). Lets the inline BusRead /
  // BusWrite fast path skip the unique_ptr<> deref (non-trivial in debug).
  SystemBus* system_bus_raw_ = nullptr;

  // Master-cycle cost of the most recent bus access (BusRead / BusWrite).
  // Populated by both the fast pointer path and the slow Plan+Follow path so
  // the micro-op retirement code in Tick can advance cycle_time by the real
  // access_speed (6 for FASTROM, 8 for slow ROM/WRAM/MMIO, 12 for joypad).
  // Meaningless for micro-ops that don't issue a bus access.
  TimeMasterDeltaT last_access_cycles_ = 0;

  // DRAM refresh state. next_refresh_time_ is the absolute master time at
  // which the next 40-cycle refresh window begins. refresh_cycles_remaining_
  // is the number of stall cycles left in the current window (0 when no
  // refresh is active).
  TimeMasterT next_refresh_time_ = kDramRefreshStartCycle;
  TimeMasterDeltaT refresh_cycles_remaining_ = 0;
  uint64_t retired_refresh_windows_ = 0;
  uint64_t retired_refresh_cycles_ = 0;

  // Partial-op bookkeeping: cycles already banked into the next micro-op that
  // hasn't executed yet. When a TickToTarget call ends mid-op (the next op
  // would overshoot the target), master_time is advanced to target and the
  // consumed cycles are stored here. On resume, the pending op's remaining
  // cost is (cost - partial_op_cycles_); the op executes when it fits.
  TimeMasterDeltaT partial_op_cycles_ = 0;

  [[nodiscard]] bool ShouldFetchInstruction() const { return micro_op_index_ == 0; }

  // Fetch/execute pipeline. All definitions in cpu.cpp.
  [[nodiscard]] StepResult FetchOpcode(TimeMasterDeltaT cycle_time);
  [[nodiscard]] StepResult ExecuteMicroOp(TimeMasterDeltaT cycle_time);
  [[nodiscard]] TickResult PerformBusAction(MicroBusAction action, [[maybe_unused]] uint8_t params,
                                            TimeMasterDeltaT cycle_time);
  // Returns 0 when the CPU can't estimate (e.g., mid-fetch); loop still forward-progresses.
  [[nodiscard]] TimeMasterDeltaT EstimateNextStepCostOrZero() const;

  // Internal-op dispatch. The switch and every op body live in cpu.cpp; we
  // keep only the declaration here so that adding a new MicroInternalOp
  // variant does not trigger a recompile of every TU that includes cpu.h.
  void ExecuteInternalOp(MicroInternalOp op, [[maybe_unused]] uint8_t params);

  void FinishInstruction();
  void DrainSkippedMicroOpsSlow();
  // Fast guard: needs_drain_ is set at instruction-entry time iff the
  // instruction has any conditional rules (rule_count > 1). For the common
  // case of unconditional instructions this is a single-load branch.
  void DrainSkippedMicroOps() {
    if (!needs_drain_) return;
    DrainSkippedMicroOpsSlow();
  }
  void RecordFault(Fault::Type type, uint8_t opcode, SnesAddrT opcode_address);

  [[nodiscard]] bool EvaluateTimingRule(uint32_t truth_table) const;

  [[nodiscard]] uint8_t ReadResetVectorByte(SnesAddrT addr);

  // Sample the /NMI line from the PPU at `t`, run the AND-gate against
  // NMITIMEN.7, update nmi_curr_ / nmi_gated_prev_ and latch nmi_pending_ on
  // a detected falling edge. Called at instruction boundaries.
  void SampleInterrupts(TimeMasterT t);

  // Choose the highest-priority pending interrupt kind, or std::nullopt if
  // none should be delivered this cycle. Priority (WDC §9): ABORT > NMI > IRQ.
  [[nodiscard]] std::optional<InterruptKind> SelectPendingInterrupt() const;

  // WAI wake predicate: any interrupt pin asserted at `t`. Samples the raw
  // /NMI line (NOT the gated flip-flop) so WAI wakes even when NMITIMEN.7 is
  // clear or I=1 — wake is not the same as delivery. ABORT / IRQ are
  // placeholder wires until those signals land.
  [[nodiscard]] bool WaiShouldWake(TimeMasterT t);

 public:
  // Called by CpuMmio when software writes $4200 and NMITIMEN.7 transitions.
  // Handles the 0→1 transparency quirk (gate becomes transparent while /NMI
  // is currently asserted → immediate NMI) and the 1→0 cancellation quirk
  // (gate closing clears the pending flip-flop). Caller passes the previous
  // and new byte values of $4200 and the master cycle of the write.
  void OnNmiTimenChanged(uint8_t prev_byte, uint8_t new_byte, TimeMasterT t);

 private:
  // Issue a Plan/Follow pair against the system bus at local_time_+cycle_time.
  // Returns the follow result; caller interprets outcome.
  [[nodiscard]] BusFollowResult PlanAndFollow(SnesAddrT addr, BusAccessType type, uint8_t data,
                                              TimeMasterDeltaT cycle_time);

  // Bus read/write. Fast-path hits system_bus_raw_->TryFastRead/Write; slow
  // path handles unmapped pages, MMIO, scheduled accesses, and test mocks.
  [[nodiscard]] TickResult BusRead(SnesAddrT addr, TimeMasterDeltaT cycle_time);
  [[nodiscard]] TickResult BusWrite(SnesAddrT addr, uint8_t data, TimeMasterDeltaT cycle_time);
  [[nodiscard]] TickResult BusReadSlow(SnesAddrT addr, TimeMasterDeltaT cycle_time);
  [[nodiscard]] TickResult BusWriteSlow(SnesAddrT addr, uint8_t data, TimeMasterDeltaT cycle_time);
};

}  // namespace pupsnes
