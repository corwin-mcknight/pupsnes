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
    case MicroBusAction::kWriteAddr:
      return "WriteAddr";
    case MicroBusAction::kWriteA8Addr:
      return "WriteA8Addr";
    case MicroBusAction::kWriteX8Addr:
      return "WriteX8Addr";
    case MicroBusAction::kWriteY8Addr:
      return "WriteY8Addr";
    case MicroBusAction::kWriteAHighAddr:
      return "WriteAHighAddr";
    case MicroBusAction::kWriteXHighAddr:
      return "WriteXHighAddr";
    case MicroBusAction::kWriteYHighAddr:
      return "WriteYHighAddr";
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
    case MicroInternalOp::kAluAdc8FromFetch:
      return "AluAdc8FromFetch";
    case MicroInternalOp::kAluSbc8FromFetch:
      return "AluSbc8FromFetch";
    case MicroInternalOp::kAluAnd8FromFetch:
      return "AluAnd8FromFetch";
    case MicroInternalOp::kAluOra8FromFetch:
      return "AluOra8FromFetch";
    case MicroInternalOp::kAluEor8FromFetch:
      return "AluEor8FromFetch";
    case MicroInternalOp::kAluCmp8FromFetch:
      return "AluCmp8FromFetch";
    case MicroInternalOp::kAluCpx8FromFetch:
      return "AluCpx8FromFetch";
    case MicroInternalOp::kAluCpy8FromFetch:
      return "AluCpy8FromFetch";
    case MicroInternalOp::kAluBit8ImmFromFetch:
      return "AluBit8ImmFromFetch";
    case MicroInternalOp::kAluAdc16FromFetch:
      return "AluAdc16FromFetch";
    case MicroInternalOp::kAluSbc16FromFetch:
      return "AluSbc16FromFetch";
    case MicroInternalOp::kAluAnd16FromFetch:
      return "AluAnd16FromFetch";
    case MicroInternalOp::kAluOra16FromFetch:
      return "AluOra16FromFetch";
    case MicroInternalOp::kAluEor16FromFetch:
      return "AluEor16FromFetch";
    case MicroInternalOp::kAluCmp16FromFetch:
      return "AluCmp16FromFetch";
    case MicroInternalOp::kAluCpx16FromFetch:
      return "AluCpx16FromFetch";
    case MicroInternalOp::kAluCpy16FromFetch:
      return "AluCpy16FromFetch";
    case MicroInternalOp::kAluBit16ImmFromFetch:
      return "AluBit16ImmFromFetch";
  }
  return "?";
}

}  // namespace pupsnes
