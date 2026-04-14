#include "pupsnes/hw/token.h"

namespace pupsnes {

token_id_t TokenTable::create(TokenType type, device_id_t source, time_master_t completion_time,
                              snes_addr_t address, uint8_t data) {
    token_id_t id = next_token_id_++;
    Token token{id, type, TokenState::Pending, source, completion_time, address, data};
    tokens_.emplace(id, token);
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

std::vector<device_id_t> TokenTable::resolveAt(time_master_t now) {
    std::vector<device_id_t> woken;

    for (auto &[id, token] : tokens_) {
        if (token.state == TokenState::Pending && token.completion_time == now) {
            token.state = TokenState::Completed;

            auto blocked_it = blocked_.find(id);
            if (blocked_it != blocked_.end()) {
                woken.push_back(blocked_it->second);
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

void TokenTable::setBlocked(token_id_t token_id, device_id_t device_id) {
    blocked_[token_id] = device_id;
}

} // namespace pupsnes
