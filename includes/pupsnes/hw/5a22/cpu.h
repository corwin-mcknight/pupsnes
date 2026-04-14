#pragma once

#include "pupsnes/hw/device.h"
#include "pupsnes/types.h"

#include <array>
#include <cstdint>
#include <optional>

namespace pupsnes {

class SNES;

// 65C816 processor status register.
// E (emulation mode) is not stored in P but is tracked here alongside it.
struct CpuFlags {
    bool N = false; // Negative
    bool V = false; // Overflow
    bool M = true;  // Memory/accumulator select: 1=8-bit (always true in emulation mode)
    bool X = true;  // Index register select: 1=8-bit (always true in emulation mode)
    bool D = false; // Decimal mode
    bool I = true;  // IRQ disable
    bool Z = false; // Zero
    bool C = false; // Carry
    bool E = true;  // Emulation mode (toggled via XCE; not a P register bit)

    [[nodiscard]] uint8_t toByte() const;
    void fromByte(uint8_t p, bool emulation_mode);
};

// Bus actions a micro-op can perform in a single master clock cycle.
enum class MicroBusAction : uint8_t {
    None,      // Internal cycle — no bus transaction
    FetchPC,   // Read byte from PBR:PC, increment PC
    ReadAddr,  // Read byte from effective address (addr_)
    WriteAddr, // Write fetch_data_ to effective address (addr_)
};

// Internal register operations performed after the bus action completes.
enum class MicroInternalOp : uint8_t {
    None,
    LoadALow_UpdateNZ, // A_lo = fetch_data_; update N/Z from A (respects M flag)
};

struct MicroOp {
    MicroBusAction bus_action = MicroBusAction::None;
    MicroInternalOp internal_op = MicroInternalOp::None;
};

// Maximum micro-ops remaining after the opcode fetch (longest 65C816 instruction = 7 cycles).
inline constexpr uint8_t MAX_REMAINING_OPS = 7;

// Per-opcode micro-op sequence (the cycles that follow the initial opcode fetch).
// Total instruction cycles = 1 (opcode fetch) + remaining_op_count.
struct InstructionEntry {
    uint8_t remaining_op_count = 0;
    std::array<MicroOp, MAX_REMAINING_OPS> ops{};
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
        uint16_t SP = 0x01FF; // Top of page 1 in emulation mode
        uint16_t DP = 0;
        uint8_t PBR = 0;
        uint8_t DBR = 0;
        uint16_t PC = 0;
        CpuFlags P{};
    };

    explicit CPU(SNES *snes);
    ~CPU() override = default;

    [[nodiscard]] TickResult tick(time_master_delta_t budget) override;
    void onEvent(const SchedulerEvent &event) override;

    [[nodiscard]] Regs regs() const { return regs_; }
    void setRegs(const Regs &r) { regs_ = r; }
    [[nodiscard]] uint8_t getMicroOpIndex() const { return micro_op_index_; }

  private:
    Regs regs_;

    // Micro-op execution state.
    uint8_t micro_op_index_ = 0; // 0 = opcode fetch; 1..N = remaining ops
    uint8_t fetch_data_ = 0;     // Last byte read from bus
    uint32_t addr_ = 0;          // Effective address accumulator

    const InstructionEntry *current_instr_ = nullptr;

    // Opcode → micro-op sequence table.  Initialized in cpu.cpp.
    static const std::array<InstructionEntry, 256> kOpcodeTable;

    void executeInternalOp(MicroInternalOp op);
    void opLoadALow_UpdateNZ();

    [[nodiscard]] snes_addr_t pcAddr() const;

    // Execute a bus read. Returns a TickResult if the access blocks (caller must return it).
    // On inline completion, writes the read byte to fetch_data_ and returns nullopt.
    // On rejected (unmapped), writes 0xFF to fetch_data_ and returns nullopt.
    [[nodiscard]] std::optional<TickResult> busRead(snes_addr_t addr, time_master_delta_t consumed);

    // Execute a bus write. Returns a TickResult if the access blocks, nullopt otherwise.
    [[nodiscard]] std::optional<TickResult> busWrite(snes_addr_t addr, uint8_t data, time_master_delta_t consumed);
};

} // namespace pupsnes
