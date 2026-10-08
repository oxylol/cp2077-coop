#include "core/Crypto.hpp"

#include <algorithm>
#include <cstring>
#include <random>
#include <string>

namespace coop::crypto
{
namespace
{
constexpr std::array<uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr uint32_t RotateRight(uint32_t aValue, int aBits)
{
    return (aValue >> aBits) | (aValue << (32 - aBits));
}
} // namespace

Sha256::Sha256()
{
    m_state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
}

void Sha256::Transform(const uint8_t* aBlock)
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
    {
        w[i] = (static_cast<uint32_t>(aBlock[i * 4]) << 24) | (static_cast<uint32_t>(aBlock[i * 4 + 1]) << 16)
             | (static_cast<uint32_t>(aBlock[i * 4 + 2]) << 8) | static_cast<uint32_t>(aBlock[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i)
    {
        const uint32_t s0 = RotateRight(w[i - 15], 7) ^ RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = RotateRight(w[i - 2], 17) ^ RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
    uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];

    for (int i = 0; i < 64; ++i)
    {
        const uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t temp1 = h + s1 + ch + kRoundConstants[static_cast<size_t>(i)] + w[i];
        const uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha256::Update(const void* aData, size_t aSize)
{
    const auto* bytes = static_cast<const uint8_t*>(aData);
    m_totalBytes += aSize;

    while (aSize > 0)
    {
        const size_t take = std::min(aSize, m_buffer.size() - m_bufferSize);
        std::memcpy(m_buffer.data() + m_bufferSize, bytes, take);
        m_bufferSize += take;
        bytes += take;
        aSize -= take;

        if (m_bufferSize == m_buffer.size())
        {
            Transform(m_buffer.data());
            m_bufferSize = 0;
        }
    }
}

Hash256 Sha256::Final()
{
    const uint64_t bitLength = m_totalBytes * 8;

    const uint8_t pad = 0x80;
    Update(&pad, 1);
    const uint8_t zero = 0;
    while (m_bufferSize != 56)
        Update(&zero, 1);

    uint8_t lengthBytes[8];
    for (int i = 0; i < 8; ++i)
        lengthBytes[i] = static_cast<uint8_t>(bitLength >> (56 - i * 8));
    Update(lengthBytes, 8);

    Hash256 digest{};
    for (size_t i = 0; i < 8; ++i)
    {
        digest[i * 4] = static_cast<uint8_t>(m_state[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(m_state[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(m_state[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(m_state[i]);
    }
    return digest;
}

Hash256 Sha256::Hash(const void* aData, size_t aSize)
{
    Sha256 hasher;
    hasher.Update(aData, aSize);
    return hasher.Final();
}

Hash256 HmacSha256(const uint8_t* aKey, size_t aKeySize, const uint8_t* aData, size_t aDataSize)
{
    std::array<uint8_t, 64> key{};
    if (aKeySize > key.size())
    {
        const auto hashed = Sha256::Hash(aKey, aKeySize);
        std::memcpy(key.data(), hashed.data(), hashed.size());
    }
    else if (aKeySize > 0)
    {
        std::memcpy(key.data(), aKey, aKeySize);
    }

    std::array<uint8_t, 64> innerPad{};
    std::array<uint8_t, 64> outerPad{};
    for (size_t i = 0; i < key.size(); ++i)
    {
        innerPad[i] = static_cast<uint8_t>(key[i] ^ 0x36);
        outerPad[i] = static_cast<uint8_t>(key[i] ^ 0x5c);
    }

    Sha256 inner;
    inner.Update(innerPad.data(), innerPad.size());
    inner.Update(aData, aDataSize);
    const auto innerHash = inner.Final();

    Sha256 outer;
    outer.Update(outerPad.data(), outerPad.size());
    outer.Update(innerHash.data(), innerHash.size());
    return outer.Final();
}

std::vector<uint8_t> Pbkdf2Sha256(std::string_view aPassword, const uint8_t* aSalt, size_t aSaltSize,
                                  uint32_t aIterations, size_t aOutputSize)
{
    std::vector<uint8_t> output;
    output.reserve(aOutputSize);

    const auto* password = reinterpret_cast<const uint8_t*>(aPassword.data());
    std::vector<uint8_t> saltBlock(aSalt, aSalt + aSaltSize);
    saltBlock.resize(aSaltSize + 4);

    for (uint32_t blockIndex = 1; output.size() < aOutputSize; ++blockIndex)
    {
        saltBlock[aSaltSize] = static_cast<uint8_t>(blockIndex >> 24);
        saltBlock[aSaltSize + 1] = static_cast<uint8_t>(blockIndex >> 16);
        saltBlock[aSaltSize + 2] = static_cast<uint8_t>(blockIndex >> 8);
        saltBlock[aSaltSize + 3] = static_cast<uint8_t>(blockIndex);

        auto u = HmacSha256(password, aPassword.size(), saltBlock.data(), saltBlock.size());
        auto t = u;
        for (uint32_t i = 1; i < aIterations; ++i)
        {
            u = HmacSha256(password, aPassword.size(), u.data(), u.size());
            for (size_t j = 0; j < t.size(); ++j)
                t[j] ^= u[j];
        }

        const size_t take = std::min(t.size(), aOutputSize - output.size());
        output.insert(output.end(), t.begin(), t.begin() + static_cast<std::ptrdiff_t>(take));
    }
    return output;
}

bool Equal(const uint8_t* aA, const uint8_t* aB, size_t aSize)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < aSize; ++i)
        diff |= static_cast<uint8_t>(aA[i] ^ aB[i]);
    return diff == 0;
}

void RandomBytes(uint8_t* aOut, size_t aSize)
{
    static thread_local std::random_device s_device;
    for (size_t i = 0; i < aSize; i += 4)
    {
        const uint32_t value = s_device();
        for (size_t j = 0; j < 4 && i + j < aSize; ++j)
            aOut[i + j] = static_cast<uint8_t>(value >> (j * 8));
    }
}

std::string ToHex(const uint8_t* aData, size_t aSize)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(aSize * 2);
    for (size_t i = 0; i < aSize; ++i)
    {
        result.push_back(kDigits[aData[i] >> 4]);
        result.push_back(kDigits[aData[i] & 0x0F]);
    }
    return result;
}
} // namespace coop::crypto
