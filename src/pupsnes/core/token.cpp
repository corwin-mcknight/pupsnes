#include "pupsnes/core/token.h"

#include <vector>

namespace pupsnes {

TokenIdT TokenTable::Create(const TokenCreateParams& params) {
  TokenIdT id = next_token_id_++;
  tokens_.emplace(id, Token{id, params.type, TokenState::kPending, params.source_device, params.completion_time,
                            params.address, params.data});
  return id;
}

const Token* TokenTable::Get(TokenIdT id) const {
  auto it = tokens_.find(id);
  if (it == tokens_.end()) {
    return nullptr;
  }
  return &it->second;
}

void TokenTable::Complete(TokenIdT id, uint8_t data) {
  auto it = tokens_.find(id);
  if (it == tokens_.end()) {
    return;
  }
  it->second.state = TokenState::kCompleted;
  it->second.data = data;
}

std::vector<TokenWake> TokenTable::ResolveAt(TimeMasterT now) {
  std::vector<TokenWake> woken;

  for (auto& [id, token] : tokens_) {
    if (token.state == TokenState::kPending && token.completion_time == now) {
      token.state = TokenState::kCompleted;

      auto blocked_it = blocked_.find(id);
      if (blocked_it != blocked_.end()) {
        woken.push_back({id, blocked_it->second});
        blocked_.erase(blocked_it);
      }
    }
  }

  return woken;
}

void TokenTable::Remove(TokenIdT id) {
  tokens_.erase(id);
  blocked_.erase(id);
}

void TokenTable::SetBlocked(TokenIdT token_id, DeviceIdT device_id) { blocked_[token_id] = device_id; }

}  // namespace pupsnes
