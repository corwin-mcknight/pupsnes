#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "pupsnes/hw/device.h"
#include "pupsnes/hw/systembus.h"
#include "pupsnes/types.h"

namespace pupsnes {

class SNES;

// 65C816 processor status register.
// E (emulation mode) is not stored in P but is tracked here alongside it.
struct CpuFlags {
  bool N = false;  // Negative
  bool V = false;  // Overflow
  bool M = true;   // Memory/accumulator select: 1=8-bit (always true in
                   // emulation mode)
  bool X = true;   // Index register select: 1=8-bit (always true in emulation mode)
  bool D = false;  // Decimal mode
  bool I = true;   // IRQ disable
  bool Z = false;  // Zero
  bool C = false;  // Carry
  bool E = true;   // Emulation mode (toggled via XCE; not a P register bit)

  [[nodiscard]] uint8_t ToByte() const;
  void FromByte(uint8_t p, bool emulation_mode);
};

// Bus actions a micro-op can perform in a single master clock cycle.
enum class MicroBusAction : uint8_t {
  kNone,         // Internal cycle — no bus transaction
  kFetchPc,      // Read byte from PBR:PC, increment PC
  kReadAddr,     // Read byte from effective address (addr_)
  kWriteAddr,    // Write fetch_data_ to effective address (addr_)
  kWriteA8Addr,  // Write A low byte to effective address (addr_)
};

// Internal register operations performed after the bus action completes.
enum class MicroInternalOp : uint8_t {
  kNone,
  kLoadALowUpdateNz,         // A_lo = fetch_data_; update N/Z from A (respects M flag)
  kLoadXLowUpdateNz,         // X_lo = fetch_data_; update N/Z from X (respects X flag)
  kSetBranchTaken,           // branch_taken = true
  kSetBranchTakenIfNotZero,  // branch_taken = !Z
  kBranchRelative8,          // Apply signed 8-bit branch offset stored in fetch_data_
                             // to PC
  kSetAddrLowFromFetch,      // addr_[7:0] = fetch_data_
  kSetAddrHighFromFetch,     // addr_[15:8] = fetch_data_
  kSetAddrBankFromFetch,     // addr_[23:16] = fetch_data_
};

enum class TimingCondition : uint8_t {
  kBranchTaken = 0,
};

enum class TimingRuleOp : uint8_t {
  kAlways = 0,
  kCondition = 1,
  kNot = 2,
  kAllOf = 3,
  kAnyOf = 4,
};

struct TimingRuleNode {
  TimingRuleOp op = TimingRuleOp::kAlways;
  TimingCondition condition = TimingCondition::kBranchTaken;
  uint8_t lhs = 0;
  uint8_t rhs = 0;
};

inline constexpr uint8_t kMaxTimingRuleNodes = 7;

struct TimingRuleExpr {
  uint8_t node_count = 0;
  uint8_t root_index = 0;
  std::array<TimingRuleNode, kMaxTimingRuleNodes> nodes{};
};

struct MicroOp {
  MicroBusAction bus_action = MicroBusAction::kNone;
  MicroInternalOp internal_op = MicroInternalOp::kNone;
  uint8_t rule_index = 0;
};

// Maximum micro-ops remaining after the opcode fetch (longest 65C816
// instruction = 7 cycles).
inline constexpr uint8_t kMaxRemainingOps = 7;
inline constexpr uint8_t kMaxInstructionRules = 4;

enum class InstructionDisposition : uint8_t {
  kImplemented = 0,
  kFaultUnimplemented = 1,
};

// Per-opcode micro-op sequence (the cycles that follow the initial opcode
// fetch). Total instruction cycles = 1 (opcode fetch) + remaining_op_count.
struct InstructionEntry {
  InstructionDisposition disposition = InstructionDisposition::kFaultUnimplemented;
  uint8_t remaining_op_count = 0;
  uint8_t rule_count = 0;
  std::array<MicroOp, kMaxRemainingOps> ops{};
  std::array<TimingRuleExpr, kMaxInstructionRules> rules{};
};

// 65C816 CPU device.
//
// tick() walks a micro-op table: cycle 0 always fetches the opcode via the
// SystemBus, then the per-opcode remaining ops execute one per cycle. All bus
// accesses use SystemBus plan/follow.
class CPU : public Device {
 public:
  struct Regs {
    uint16_t A = 0;
    uint16_t X = 0;
    uint16_t Y = 0;
    uint16_t SP = 0x01FF;  // Top of page 1 in emulation mode
    uint16_t DP = 0;
    uint8_t PBR = 0;
    uint8_t DBR = 0;
    uint16_t PC = 0;
    CpuFlags P{};
  };

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

  void Reset();

  [[nodiscard]] TickResult Tick(TimeMasterDeltaT budget) override;
  void OnEvent(const SchedulerEvent& event) override;

  [[nodiscard]] struct Regs GetRegs() const { return regs_; }
  void SetRegs(const Regs& r) { regs_ = r; }
  [[nodiscard]] uint8_t GetMicroOpIndex() const { return micro_op_index_; }
  [[nodiscard]] const std::optional<Fault>& GetFault() const { return fault_; }

 private:
  struct TimingContext {
    bool branch_taken = false;
  };

  Regs regs_;

  // Micro-op execution state.
  // micro_op_index_ == 0 means we are between instructions (next cycle fetches
  // the opcode); 1..N indexes into the current instruction's remaining ops.
  uint8_t micro_op_index_ = 0;
  uint8_t fetch_data_ = 0;  // Last byte read from bus
  uint32_t addr_ = 0;       // Effective address accumulator
  TimingContext timing_context_{};
  std::optional<Fault> fault_ = std::nullopt;

  const InstructionEntry* current_instr_ = nullptr;

  // Opcode → micro-op sequence table.  Defined in cpu_opcodes.cpp.
  static const std::array<InstructionEntry, 256> kOpcodeTable;

  [[nodiscard]] bool ShouldFetchInstruction() const { return micro_op_index_ == 0; }

  struct StepResult {
    bool consumed_cycle = false;
    std::optional<TickResult> stop = std::nullopt;
  };

  // Tick helpers: one per branch of the fetch/execute loop. Each returns a
  // TickResult if the access blocks (caller must return it).
  [[nodiscard]] StepResult FetchOpcode(TimeMasterDeltaT cycle_time);
  [[nodiscard]] StepResult ExecuteMicroOp(TimeMasterDeltaT cycle_time);
  [[nodiscard]] std::optional<TickResult> PerformBusAction(MicroBusAction action, TimeMasterDeltaT cycle_time);

  void ExecuteInternalOp(MicroInternalOp op);
  void OpLoadALowUpdateNz();
  void OpLoadXLowUpdateNz();
  void OpSetBranchTaken(bool taken);
  void OpSetBranchTakenIfNotZero();
  void OpBranchRelative8();
  // Replace byte [shift, shift+7] of addr_ with fetch_data_.
  void SetAddrByteFromFetch(unsigned shift);
  void FinishInstruction();
  void DrainSkippedMicroOps();
  void RecordFault(Fault::Type type, uint8_t opcode, SnesAddrT opcode_address);
  [[nodiscard]] bool EvaluateTimingCondition(TimingCondition condition) const;
  [[nodiscard]] bool EvaluateTimingRule(const TimingRuleExpr& rule) const;
  [[nodiscard]] bool ShouldExecuteMicroOp(const MicroOp& op) const;

  [[nodiscard]] uint8_t ReadResetVectorByte(SnesAddrT addr);

  [[nodiscard]] SnesAddrT PcAddr() const;

  // Issue a Plan/Follow pair against the system bus at local_time_+cycle_time.
  // Returns the follow result; caller interprets outcome.
  [[nodiscard]] BusFollowResult PlanAndFollow(SnesAddrT addr, BusAccessType type, uint8_t data,
                                              TimeMasterDeltaT cycle_time);

  // Execute a bus read. Returns a TickResult if the access blocks (caller must
  // return it). On inline completion, writes the read byte to fetch_data_ and
  // returns nullopt. On rejected (unmapped), writes 0xFF to fetch_data_ and
  // returns nullopt.
  [[nodiscard]] std::optional<TickResult> BusRead(SnesAddrT addr, TimeMasterDeltaT cycle_time);

  // Execute a bus write. Returns a TickResult if the access blocks, nullopt
  // otherwise.
  [[nodiscard]] std::optional<TickResult> BusWrite(SnesAddrT addr, uint8_t data, TimeMasterDeltaT cycle_time);
};

}  // namespace pupsnes
