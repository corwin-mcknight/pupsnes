#include "pupsnes/hw/apu_stub.h"

#include "pupsnes/hw/snes.h"

namespace pupsnes {

ApuStub::ApuStub(SNES* snes) : Device(snes) {}

void ApuStub::ArmSignature() {
  ports_[0] = kResetPort0;
  ports_[1] = kResetPort1;
  ports_[2] = 0;
  ports_[3] = 0;
  signature_consumed_ = false;
  read_streak_ = 0;
}

void ApuStub::Reset() { ArmSignature(); }

MmioReadResult ApuStub::ReadRegister(uint32_t offset, TimeMasterT current_time) {
  const std::size_t port = offset & 0x3U;
  if (!signature_consumed_) {
    if (current_time < kSignatureDelayMaster) {
      // SPC700 IPL ROM hasn't finished its post-reset boot — ports read $00.
      return {0x00U, 0xFFU};
    }
    return {ports_[port], 0xFFU};
  }
  ++read_streak_;
  if (read_streak_ >= kStallReadThreshold) {
    // Stuck read streak — almost certainly the CPU spinning on a
    // signature-wait loop after an upload-block executed. Re-arm $AA/$BB so
    // the loop's next iteration unblocks. Real hardware achieves this via
    // the uploaded SPC program writing $AA/$BB back to $F4/$F5 itself. No
    // boot-delay re-application here: the SPC has been running for a long
    // time, so the next read should see the signature immediately.
    ArmSignature();
    return {ports_[port], 0xFFU};
  }
  return {ports_[port], 0xFFU};
}

void ApuStub::WriteRegister(uint32_t offset, uint8_t data, TimeMasterT /*current_time*/) {
  const std::size_t port = offset & 0x3U;
  read_streak_ = 0;
  if (!signature_consumed_) {
    // Only the IPL kick byte ($CC on port 0) leaves signature mode. Any other
    // write — including memory-clear loops that STZ through the APU port
    // range before the CPU has checked the signature — is dropped so port 0/1
    // keep reading $AA/$BB.
    if (port == 0 && data == kUploadKickByte) {
      signature_consumed_ = true;
      ports_[0] = kUploadKickByte;  // echo the kick as the ack the CPU spins on
    }
    return;
  }
  // Echo mode: every CPU write flows straight into the read latch so the
  // per-byte handshake's `write counter → CMP against counter` loop sees its
  // own value on the next read.
  ports_[port] = data;
}

std::optional<uint8_t> ApuStub::HandleDebugRead(uint32_t offset) const { return ports_[offset & 0x3U]; }

bool ApuStub::HandleDebugWrite(uint32_t /*offset*/, uint8_t /*data*/) { return true; }

}  // namespace pupsnes
