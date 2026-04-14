#include "pupsnes/hw/5a22/cpu.h"

#include "pupsnes/hw/scheduler.h"
#include "pupsnes/hw/snes.h"
#include "pupsnes/hw/systembus.h"

#include <array>
#include <cstdint>
#include <optional>

namespace pupsnes {

// ---------------------------------------------------------------------------
// CpuFlags
// ---------------------------------------------------------------------------

uint8_t CpuFlags::toByte() const {
    uint8_t p = 0;
    if (N)
        p |= 0x80U;
    if (V)
        p |= 0x40U;
    if (M)
        p |= 0x20U;
    if (X)
        p |= 0x10U;
    if (D)
        p |= 0x08U;
    if (I)
        p |= 0x04U;
    if (Z)
        p |= 0x02U;
    if (C)
        p |= 0x01U;
    return p;
}

void CpuFlags::fromByte(uint8_t p, bool emulation_mode) {
    N = (p & 0x80U) != 0U;
    V = (p & 0x40U) != 0U;
    // In emulation mode M and X are forced to 1 and cannot be changed via P.
    if (!emulation_mode) {
        M = (p & 0x20U) != 0U;
        X = (p & 0x10U) != 0U;
    }
    D = (p & 0x08U) != 0U;
    I = (p & 0x04U) != 0U;
    Z = (p & 0x02U) != 0U;
    C = (p & 0x01U) != 0U;
}

// ---------------------------------------------------------------------------
// Opcode table
// All 256 opcodes default to NOP timing: 1 remaining internal cycle.
// Group-init helpers populate specific opcodes by instruction family.
// ---------------------------------------------------------------------------

namespace {

void initMiscOpcodes(std::array<InstructionEntry, 256> &table) {
    // 0xEA  NOP  — 2 cycles; explicit for clarity (same as default).
    table[0xEA].remaining_op_count = 1;
    table[0xEA].ops[0] = {MicroBusAction::None, MicroInternalOp::None};
}

void initLoadOpcodes(std::array<InstructionEntry, 256> &table) {
    // 0xA9  LDA #imm  — 2 cycles (8-bit accumulator mode).
    // Remaining op: fetch immediate byte and load into A, update N/Z.
    table[0xA9].remaining_op_count = 1;
    table[0xA9].ops[0] = {MicroBusAction::FetchPC, MicroInternalOp::LoadALow_UpdateNZ};
}

} // namespace

const std::array<InstructionEntry, 256> CPU::kOpcodeTable = []() {
    std::array<InstructionEntry, 256> table{};

    // Default: 2-cycle instruction (opcode fetch + 1 internal cycle, no bus action).
    for (auto &entry : table) {
        entry.remaining_op_count = 1;
        entry.ops[0] = {MicroBusAction::None, MicroInternalOp::None};
    }

    initMiscOpcodes(table);
    initLoadOpcodes(table);

    return table;
}();

// ---------------------------------------------------------------------------
// CPU
// ---------------------------------------------------------------------------

CPU::CPU(SNES *snes) : Device(snes) {}

void CPU::opLoadALow_UpdateNZ() {
    regs_.A = static_cast<uint16_t>((regs_.A & 0xFF00U) | fetch_data_);
    regs_.P.Z = (static_cast<uint8_t>(regs_.A) == 0U);
    regs_.P.N = (regs_.A & 0x0080U) != 0U;
}

void CPU::executeInternalOp(MicroInternalOp op) {
    switch (op) {
    case MicroInternalOp::None:
        break;
    case MicroInternalOp::LoadALow_UpdateNZ:
        opLoadALow_UpdateNZ();
        break;
    }
}

std::optional<TickResult> CPU::busRead(snes_addr_t addr, time_master_delta_t consumed) {
    auto plan = snes->system_bus->plan(addr, BusAccessType::Read);
    auto result = snes->system_bus->follow(plan, local_time, device_id_);

    if (result.outcome == BusPlanOutcome::ScheduledComplete) {
        return TickResult{consumed, TickStopReason::BlockedOnToken, result.token};
    }

    fetch_data_ = result.data;
    return std::nullopt;
}

std::optional<TickResult> CPU::busWrite(snes_addr_t addr, uint8_t data, time_master_delta_t consumed) {
    auto plan = snes->system_bus->plan(addr, BusAccessType::Write, data);
    auto result = snes->system_bus->follow(plan, local_time, device_id_);

    if (result.outcome == BusPlanOutcome::ScheduledComplete) {
        return TickResult{consumed, TickStopReason::BlockedOnToken, result.token};
    }

    return std::nullopt;
}

snes_addr_t CPU::pcAddr() const { return (static_cast<uint32_t>(regs_.PBR) << 16U) | static_cast<uint32_t>(regs_.PC); }

TickResult CPU::tick(time_master_delta_t budget) {
    time_master_delta_t consumed = 0;

    while (consumed < budget) {
        if (micro_op_index_ == 0) {
            // Cycle 0 of every instruction: fetch opcode from PBR:PC.
            if (auto blocked = busRead(pcAddr(), consumed)) {
                return *blocked;
            }
            regs_.PC = static_cast<uint16_t>(regs_.PC + 1U);

            current_instr_ = &kOpcodeTable[fetch_data_];
            micro_op_index_ = 1;
        } else {
            // Cycles 1..N: execute the opcode's remaining micro-ops.
            const uint8_t op_idx = static_cast<uint8_t>(micro_op_index_ - 1U);

            if (current_instr_ == nullptr || op_idx >= current_instr_->remaining_op_count) {
                current_instr_ = nullptr;
                micro_op_index_ = 0;
                continue;
            }

            const MicroOp &mop = current_instr_->ops[op_idx];

            switch (mop.bus_action) {
            case MicroBusAction::FetchPC: {
                if (auto blocked = busRead(pcAddr(), consumed)) {
                    return *blocked;
                }
                regs_.PC = static_cast<uint16_t>(regs_.PC + 1U);
                break;
            }
            case MicroBusAction::ReadAddr: {
                if (auto blocked = busRead(addr_, consumed)) {
                    return *blocked;
                }
                break;
            }
            case MicroBusAction::WriteAddr: {
                if (auto blocked = busWrite(addr_, fetch_data_, consumed)) {
                    return *blocked;
                }
                break;
            }
            case MicroBusAction::None:
                break;
            }

            executeInternalOp(mop.internal_op);
            micro_op_index_++;
        }

        consumed++;
        local_time++;
    }

    return {consumed, TickStopReason::BudgetExhausted, 0};
}

void CPU::onEvent(const SchedulerEvent & /*event*/) {
    // TODO: handle CommitComplete (token wake) and WakeSample (signal sampling).
}

} // namespace pupsnes
