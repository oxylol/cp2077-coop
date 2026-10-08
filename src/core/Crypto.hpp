#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/Types.hpp"

namespace coop::crypto
{
// Minimal primitives for the session password handshake (docs/01-architecture.md §5.3).
// The channel itself is encrypted by GameNetworkingSockets; these only prove knowledge of the password.

class Sha256
{
public:
    Sha256();
    void Update(const void* aData, size_t aSize);
    Hash256 Final();

    static Hash256 Hash(const void* aData, size_t aSize);

private:
    void Transform(const uint8_t* aBlock);

    std::array<uint32_t, 8> m_state{};
    std::array<uint8_t, 64> m_buffer{};
    uint64_t m_totalBytes = 0;
    size_t m_bufferSize = 0;
};

Hash256 HmacSha256(const uint8_t* aKey, size_t aKeySize, const uint8_t* aData, size_t aDataSize);

// PBKDF2-HMAC-SHA256 (RFC 8018).
std::vector<uint8_t> Pbkdf2Sha256(std::string_view aPassword, const uint8_t* aSalt, size_t aSaltSize,
                                  uint32_t aIterations, size_t aOutputSize);

// Constant-time comparison.
bool Equal(const uint8_t* aA, const uint8_t* aB, size_t aSize);

// Fills the buffer with random bytes from the OS (std::random_device).
void RandomBytes(uint8_t* aOut, size_t aSize);

std::string ToHex(const uint8_t* aData, size_t aSize);
} // namespace coop::crypto
