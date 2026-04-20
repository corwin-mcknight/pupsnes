#include "pupsnes/hw/5a22/micro_op_strings.h"

namespace pupsnes {

std::string_view ToString(MicroBusAction action) {
  switch (action) {
    case MicroBusAction::kNone:
      return "None";
    case MicroBusAction::kFetchPc:
      return "FetchPc";
    case MicroBusAction::kReadAddr:
      return "ReadAddr";
    case MicroBusAction::kWriteRegByte:
      return "WriteRegByte";
    case MicroBusAction::kPushStack:
      return "PushStack";
    case MicroBusAction::kPullStack:
      return "PullStack";
    case MicroBusAction::kPreIncPullStack:
      return "PreIncPullStack";
  }
  return "?";
}

std::string_view ToString(MicroInternalOp op) {
  switch (op) {
    case MicroInternalOp::kNone:
      return "None";
    case MicroInternalOp::kLoadReg:
      return "LoadReg";
    case MicroInternalOp::kSetBranchTakenCond:
      return "SetBranchTakenCond";
    case MicroInternalOp::kBranchRelative8:
      return "BranchRelative8";
    case MicroInternalOp::kSetAddrLowFromFetch:
      return "SetAddrLowFromFetch";
    case MicroInternalOp::kSetAddrHighFromFetch:
      return "SetAddrHighFromFetch";
    case MicroInternalOp::kSetAddrBankFromFetch:
      return "SetAddrBankFromFetch";
    case MicroInternalOp::kSetAddrHighFromFetchAndBankFromDbr:
      return "SetAddrHighFromFetchAndBankFromDbr";
    case MicroInternalOp::kIncrementAddr:
      return "IncrementAddr";
    case MicroInternalOp::kDecrementSp:
      return "DecrementSp";
    case MicroInternalOp::kIncrementSp:
      return "IncrementSp";
    case MicroInternalOp::kLoadDbrUpdateNz:
      return "LoadDbrUpdateNz";
    case MicroInternalOp::kIncDecReg:
      return "IncDecReg";
    case MicroInternalOp::kSetFlag:
      return "SetFlag";
    case MicroInternalOp::kRepFromFetch:
      return "RepFromFetch";
    case MicroInternalOp::kSepFromFetch:
      return "SepFromFetch";
    case MicroInternalOp::kExchangeCarryEmulation:
      return "ExchangeCarryEmulation";
    case MicroInternalOp::kTransferReg:
      return "TransferReg";
    case MicroInternalOp::kBranchRelative16:
      return "BranchRelative16";
    case MicroInternalOp::kSetPcFromAddr:
      return "SetPcFromAddr";
    case MicroInternalOp::kSetPcAndPbrFromAddr:
      return "SetPcAndPbrFromAddr";
    case MicroInternalOp::kSetAddrHighFromFetchAndSetPc:
      return "SetAddrHighFromFetchAndSetPc";
    case MicroInternalOp::kSetAddrBankFromFetchAndSetPcAndPbr:
      return "SetAddrBankFromFetchAndSetPcAndPbr";
    case MicroInternalOp::kDecrementPc:
      return "DecrementPc";
    case MicroInternalOp::kIncrementPc:
      return "IncrementPc";
    case MicroInternalOp::kSetPclFromFetch:
      return "SetPclFromFetch";
    case MicroInternalOp::kSetPchFromFetch:
      return "SetPchFromFetch";
    case MicroInternalOp::kSetPbrFromFetch:
      return "SetPbrFromFetch";
    case MicroInternalOp::kLoadPFromFetch:
      return "LoadPFromFetch";
    case MicroInternalOp::kLoadDpLowFromFetch:
      return "LoadDpLowFromFetch";
    case MicroInternalOp::kLoadDpHighFromFetchUpdateNz:
      return "LoadDpHighFromFetchUpdateNz";
    case MicroInternalOp::kLoadXHighFromFetchUpdateNz:
      return "LoadXHighFromFetchUpdateNz";
    case MicroInternalOp::kLoadYHighFromFetchUpdateNz:
      return "LoadYHighFromFetchUpdateNz";
    case MicroInternalOp::kAlu8Imm:
      return "Alu8Imm";
    case MicroInternalOp::kAlu16Imm:
      return "Alu16Imm";
  }
  return "?";
}

}  // namespace pupsnes
