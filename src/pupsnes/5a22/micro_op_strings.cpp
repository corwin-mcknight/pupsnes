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
    case MicroBusAction::kPushA8:
      return "PushA8";
    case MicroBusAction::kPushAHigh:
      return "PushAHigh";
    case MicroBusAction::kPushDbr:
      return "PushDbr";
    case MicroBusAction::kPullStack:
      return "PullStack";
  }
  return "?";
}

std::string_view ToString(MicroInternalOp op) {
  switch (op) {
    case MicroInternalOp::kNone:
      return "None";
    case MicroInternalOp::kLoadA8UpdateNz:
      return "LoadA8UpdateNz";
    case MicroInternalOp::kLoadALow:
      return "LoadALow";
    case MicroInternalOp::kLoadAHighUpdateNz:
      return "LoadAHighUpdateNz";
    case MicroInternalOp::kLoadX8UpdateNz:
      return "LoadX8UpdateNz";
    case MicroInternalOp::kLoadXLow:
      return "LoadXLow";
    case MicroInternalOp::kLoadXHighUpdateNz:
      return "LoadXHighUpdateNz";
    case MicroInternalOp::kLoadY8UpdateNz:
      return "LoadY8UpdateNz";
    case MicroInternalOp::kLoadYLow:
      return "LoadYLow";
    case MicroInternalOp::kLoadYHighUpdateNz:
      return "LoadYHighUpdateNz";
    case MicroInternalOp::kSetBranchTaken:
      return "SetBranchTaken";
    case MicroInternalOp::kSetBranchTakenIfNotZero:
      return "SetBranchTakenIfNotZero";
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
    case MicroInternalOp::kIncA:
      return "IncA";
    case MicroInternalOp::kDecA:
      return "DecA";
    case MicroInternalOp::kIncX:
      return "IncX";
    case MicroInternalOp::kDecX:
      return "DecX";
    case MicroInternalOp::kIncY:
      return "IncY";
    case MicroInternalOp::kDecY:
      return "DecY";
  }
  return "?";
}

}  // namespace pupsnes
