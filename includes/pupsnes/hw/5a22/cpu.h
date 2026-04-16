#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "pupsnes/hw/device.h"
#include "pupsnes/types.h"

namespace pupsnes {

class SNES;

// 65C816 processor status register.
// E (emulation mode) is not stored in P but is tracked here alongside it.
struct CpuFlags {
    bool N = false;  // Negative
    bool V = false;  // Overflow
    bool M = true;   // Memory/accumulator select: 1=8-bit (always true in emulation mode)
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
    kLoadALowUpdateNz,      // A_lo = fetch_data_; update N/Z from A (respects M flag)
    kBranchRelative8,       // Apply signed 8-bit branch offset stored in fetch_data_ to PC
    kSetAddrLowFromFetch,   // addr_[7:0] = fetch_data_
    kSetAddrHighFromFetch,  // addr_[15:8] = fetch_data_
    kSetAddrBankFromFetch,  // addr_[23:16] = fetch_data_
};

struct MicroOp {
    MicroBusAction bus_action = MicroBusAction::kNone;
    MicroInternalOp internal_op = MicroInternalOp::kNone;
};

// Maximum micro-ops remaining after the opcode fetch (longest 65C816 instruction = 7 cycles).
inline constexpr uint8_t kMaxRemainingOps = 7;

// Per-opcode micro-op sequence (the cycles that follow the initial opcode fetch).
// Total instruction cycles = 1 (opcode fetch) + remaining_op_count.
struct InstructionEntry {
    uint8_t remaining_op_count = 0;
    std::array<MicroOp, kMaxRemainingOps> ops{};
};

// 65C816 CPU device.
//
// tick() walks a micro-op table: cycle 0 always fetches the opcode via the SystemBus, then
// the per-opcode remaining ops execute one per cycle. All bus accesses use SystemBus plan/follow.
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

    explicit CPU(SNES* snes);
    ~CPU() override = default;

    void Reset();

    [[nodiscard]] TickResult Tick(TimeMasterDeltaT budget) override;
    void OnEvent(const SchedulerEvent& event) override;

    [[nodiscard]] struct Regs GetRegs() const { return regs_; }
    void SetRegs(const Regs& r) { regs_ = r; }
    [[nodiscard]] uint8_t GetMicroOpIndex() const { return micro_op_index_; }

   private:
    Regs regs_;

    // Micro-op execution state.
    uint8_t micro_op_index_ = 0;  // 0 = opcode fetch; 1..N = remaining ops
    uint8_t fetch_data_ = 0;      // Last byte read from bus
    uint32_t addr_ = 0;           // Effective address accumulator

    const InstructionEntry* current_instr_ = nullptr;

    // Opcode → micro-op sequence table.  Initialized in cpu.cpp.
    static const std::array<InstructionEntry, 256> kOpcodeTable;

    void ExecuteInternalOp(MicroInternalOp op);
    void OpLoadALowUpdateNz();
    void OpBranchRelative8();
    void OpSetAddrLowFromFetch();
    void OpSetAddrHighFromFetch();
    void OpSetAddrBankFromFetch();
    [[nodiscard]] uint8_t ReadResetVectorByte(SnesAddrT addr);

    [[nodiscard]] SnesAddrT PcAddr() const;

    // Execute a bus read. Returns a TickResult if the access blocks (caller must return it).
    // On inline completion, writes the read byte to fetch_data_ and returns nullopt.
    // On rejected (unmapped), writes 0xFF to fetch_data_ and returns nullopt.
    [[nodiscard]] std::optional<TickResult> BusRead(SnesAddrT addr, TimeMasterDeltaT cycle_time);

    // Execute a bus write. Returns a TickResult if the access blocks, nullopt otherwise.
    [[nodiscard]] std::optional<TickResult> BusWrite(SnesAddrT addr, uint8_t data, TimeMasterDeltaT cycle_time);
};

}  // namespace pupsnes
