#include "pupsnes/hw/token.h"

namespace pupsnes {

token_id_t TokenTable::create(const TokenCreateParams &params) {
    token_id_t id = next_token_id_++;
    tokens_.emplace(id, Token{id, params.type, TokenState::Pending, params.source_device, params.completion_time,
                              params.address, params.data});
    return id;
}

const Token *TokenTable::get(token_id_t id) const {
    auto it = tokens_.find(id);
    if (it == tokens_.end()) {
        return nullptr;
    }
    return &it->second;
}

void TokenTable::complete(token_id_t id, uint8_t data) {
    auto it = tokens_.find(id);
    if (it == tokens_.end()) {
        return;
    }
    it->second.state = TokenState::Completed;
    it->second.data = data;
}

std::vector<TokenWake> TokenTable::resolveAt(time_master_t now) {
    std::vector<TokenWake> woken;

    for (auto &[id, token] : tokens_) {
        if (token.state == TokenState::Pending && token.completion_time == now) {
            token.state = TokenState::Completed;

            auto blocked_it = blocked_.find(id);
            if (blocked_it != blocked_.end()) {
                woken.push_back({id, blocked_it->second});
                blocked_.erase(blocked_it);
            }
        }
    }

    return woken;
}

void TokenTable::remove(token_id_t id) {
    tokens_.erase(id);
    blocked_.erase(id);
}

void TokenTable::setBlocked(token_id_t token_id, device_id_t device_id) { blocked_[token_id] = device_id; }

} // namespace pupsnes
