#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "pupsnes/core/types.h"

namespace pupsnes {

enum class TokenType : uint8_t {
  kBusRead = 0,
  kBusWrite = 1,
};

enum class TokenState : uint8_t {
  kPending = 0,
  kCompleted = 1,
};

struct Token {
  TokenIdT id;
  TokenType type;
  TokenState state;
  DeviceIdT source_device;
  TimeMasterT completion_time;
  SnesAddrT address;
  uint8_t data;
};

struct TokenCreateParams {
  TokenType type;
  DeviceIdT source_device;
  TimeMasterT completion_time;
  SnesAddrT address;
  uint8_t data = 0;
};

struct TokenWake {
  TokenIdT token_id;
  DeviceIdT device_id;
};

class TokenTable {
 public:
  TokenTable() = default;

  TokenIdT Create(const TokenCreateParams& params);

  [[nodiscard]] const Token* Get(TokenIdT id) const;

  void Complete(TokenIdT id, uint8_t data);

  /// Resolve all pending tokens whose completion_time == now.
  /// Returns token/device pairs that were blocked and should be considered for
  /// wake.
  std::vector<TokenWake> ResolveAt(TimeMasterT now);

  void Remove(TokenIdT id);

  /// Record that a device is blocked waiting on a token.
  void SetBlocked(TokenIdT token_id, DeviceIdT device_id);

 private:
  std::unordered_map<TokenIdT, Token> tokens_;
  std::unordered_map<TokenIdT, DeviceIdT> blocked_;
  uint64_t next_token_id_ = 1;
};

}  // namespace pupsnes
