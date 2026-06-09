#include "pupsnes/hw/5a22/micro_op_strings.h"

#include "pupsnes/hw/5a22/micro_op.h"

namespace pupsnes {

std::string_view ToString(MicroBusAction action) {
  switch (action) {
    case MicroBusAction::kNone: return "None";
    case MicroBusAction::kFetchPc: return "FetchPc";
    case MicroBusAction::kReadAddr: return "ReadAddr";
    case MicroBusAction::kWriteRegByte: return "WriteRegByte";
    case MicroBusAction::kPushStack: return "PushStack";
    case MicroBusAction::kPullStack: return "PullStack";
    case MicroBusAction::kPreIncPullStack: return "PreIncPullStack";
  }
  return "?";
}

std::string_view ToString(MicroInternalOp op) {
  switch (op) {
    case MicroInternalOp::kNone: return "None";
    case MicroInternalOp::kLoadReg: return "LoadReg";
    case MicroInternalOp::kLoadPcLowFromFetch: return "LoadPcLowFromFetch";
    case MicroInternalOp::kLoadPcHighFromFetch: return "LoadPcHighFromFetch";
    case MicroInternalOp::kLoadPbrFromFetch: return "LoadPbrFromFetch";
    case MicroInternalOp::kSetBranchTakenCond: return "SetBranchTakenCond";
    case MicroInternalOp::kBranchRelative: return "BranchRelative";
    case MicroInternalOp::kAddIndexToAddr: return "AddIndexToAddr";
    case MicroInternalOp::kSetAddrHighDbrAddIndex: return "SetAddrHighDbrAddIndex";
    case MicroInternalOp::kSetAddrBankFromFetchAddX: return "SetAddrBankFromFetchAddX";
    case MicroInternalOp::kSetAddrFromDp: return "SetAddrFromDp";
    case MicroInternalOp::kSetAddrFromSp: return "SetAddrFromSp";
    case MicroInternalOp::kStashIndirectLow: return "StashIndirectLow";
    case MicroInternalOp::kStashIndirectHigh: return "StashIndirectHigh";
    case MicroInternalOp::kStashOperandLow: return "StashOperandLow";
    case MicroInternalOp::kStashDpIndirectLow: return "StashDpIndirectLow";
    case MicroInternalOp::kStashDpXIndirectLow: return "StashDpXIndirectLow";
    case MicroInternalOp::kFormAddrFromScratchDbr: return "FormAddrFromScratchDbr";
    case MicroInternalOp::kFormAddrFromScratchBank: return "FormAddrFromScratchBank";
    case MicroInternalOp::kSetAddrByteFromFetch: return "SetAddrByteFromFetch";
    case MicroInternalOp::kModifyAddr: return "ModifyAddr";
    case MicroInternalOp::kModifySp: return "ModifySp";
    case MicroInternalOp::kModifyPc: return "ModifyPc";
    case MicroInternalOp::kIncDecReg: return "IncDecReg";
    case MicroInternalOp::kSetFlag: return "SetFlag";
    case MicroInternalOp::kMaskStatus: return "MaskStatus";
    case MicroInternalOp::kExchangeCarryEmulation: return "ExchangeCarryEmulation";
    case MicroInternalOp::kSwapBA: return "SwapBA";
    case MicroInternalOp::kAddPcToAddr: return "AddPcToAddr";
    case MicroInternalOp::kSetInterruptVector: return "SetInterruptVector";
    case MicroInternalOp::kEnterInterruptHandler: return "EnterInterruptHandler";
    case MicroInternalOp::kHaltCpu: return "HaltCpu";
    case MicroInternalOp::kMoveSetDbr: return "MoveSetDbr";
    case MicroInternalOp::kMoveSetAddrFromSrc: return "MoveSetAddrFromSrc";
    case MicroInternalOp::kMoveSetAddrFromDst: return "MoveSetAddrFromDst";
    case MicroInternalOp::kMoveAdjust: return "MoveAdjust";
    case MicroInternalOp::kMoveLoopCheck: return "MoveLoopCheck";
    case MicroInternalOp::kTransferReg: return "TransferReg";
    case MicroInternalOp::kLoadAddrByteAndSetPc: return "LoadAddrByteAndSetPc";
    case MicroInternalOp::kAlu8Imm: return "Alu8Imm";
    case MicroInternalOp::kAlu16Imm: return "Alu16Imm";
    case MicroInternalOp::kShiftRotateA: return "ShiftRotateA";
    case MicroInternalOp::kSetPcFromScratchAndFetch: return "SetPcFromScratchAndFetch";
    case MicroInternalOp::kRmwMem: return "RmwMem";
  }
  return "?";
}

}  // namespace pupsnes
