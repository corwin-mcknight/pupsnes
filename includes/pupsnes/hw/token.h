#pragma once

#include "pupsnes/types.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace pupsnes {

enum class TokenType : uint8_t {
    BusRead = 0,
    BusWrite = 1,
};

enum class TokenState : uint8_t {
    Pending = 0,
    Completed = 1,
};

struct Token {
    token_id_t id;
    TokenType type;
    TokenState state;
    device_id_t source_device;
    time_master_t completion_time;
    snes_addr_t address;
    uint8_t data;
};

class TokenTable {
  public:
    TokenTable() = default;

    token_id_t create(TokenType type, device_id_t source, time_master_t completion_time,
                      snes_addr_t address, uint8_t data);

    [[nodiscard]] const Token *get(token_id_t id) const;

    void complete(token_id_t id, uint8_t data);

    /// Resolve all pending tokens whose completion_time == now.
    /// Returns device IDs that were blocked and should be woken.
    std::vector<device_id_t> resolveAt(time_master_t now);

    void remove(token_id_t id);

    /// Record that a device is blocked waiting on a token.
    void setBlocked(token_id_t token_id, device_id_t device_id);

  private:
    std::unordered_map<token_id_t, Token> tokens_;
    std::unordered_map<token_id_t, device_id_t> blocked_;
    uint64_t next_token_id_ = 1;
};

} // namespace pupsnes
