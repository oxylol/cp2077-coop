#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "core/BitStream.hpp"
#include "protocol/Protocol.hpp"

namespace coop
{
struct EnvelopeHeader
{
    MsgId id{};
    uint16_t flags = 0;
};

// Reads the 4-byte envelope header. Returns false if the buffer is too small.
bool PeekHeader(const uint8_t* aData, size_t aSize, EnvelopeHeader& aOut);

void WriteHeader(std::vector<uint8_t>& aOut, MsgId aId, uint16_t aFlags);

// Serializes a message into an envelope. Returns an empty vector if the message violates its limits
// (e.g. an oversized string), which callers treat as a programming error.
template<typename M>
std::vector<uint8_t> Encode(const M& aMessage, uint16_t aFlags = 0)
{
    M copy = aMessage; // Serialize() takes mutable references in both directions
    WriteStream stream;
    if (!copy.Serialize(stream))
        return {};

    std::vector<uint8_t> out;
    const auto payload = stream.Finish();
    out.reserve(kEnvelopeSize + payload.size());
    WriteHeader(out, M::kId, aFlags);
    out.insert(out.end(), payload.begin(), payload.end());
    if (out.size() > kMaxMessageSize)
        return {};
    return out;
}

// Decodes the payload of an envelope whose header id matches M::kId.
template<typename M>
bool Decode(const uint8_t* aData, size_t aSize, M& aOut)
{
    EnvelopeHeader header;
    if (!PeekHeader(aData, aSize, header) || header.id != M::kId)
        return false;
    ReadStream stream(aData + kEnvelopeSize, aSize - kEnvelopeSize);
    return aOut.Serialize(stream);
}

template<typename M>
bool Decode(const std::vector<uint8_t>& aData, M& aOut)
{
    return Decode(aData.data(), aData.size(), aOut);
}
} // namespace coop
