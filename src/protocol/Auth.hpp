#pragma once

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

#include "core/Crypto.hpp"
#include "core/Types.hpp"

namespace coop::auth
{
inline constexpr uint32_t kDefaultIterations = 100'000;

// Password key shared by host and client: PBKDF2-HMAC-SHA256(password, salt, iterations), 32 bytes.
inline std::vector<uint8_t> DeriveKey(std::string_view aPassword, const std::array<uint8_t, 16>& aSalt,
                                      uint32_t aIterations)
{
    return crypto::Pbkdf2Sha256(aPassword, aSalt.data(), aSalt.size(), aIterations, 32);
}

// Proof of the password for one join attempt: HMAC-SHA256(key, nonce || clientId).
inline Hash256 Proof(const std::vector<uint8_t>& aKey, const std::array<uint8_t, 32>& aNonce, const Uuid& aClientId)
{
    std::array<uint8_t, 48> message{};
    std::copy(aNonce.begin(), aNonce.end(), message.begin());
    std::copy(aClientId.begin(), aClientId.end(), message.begin() + 32);
    return crypto::HmacSha256(aKey.data(), aKey.size(), message.data(), message.size());
}
} // namespace coop::auth
