#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "protocol/Protocol.hpp"

namespace coop
{
using ConnId = uint32_t;
inline constexpr ConnId kInvalidConn = 0;

enum class ConnEventType
{
    Incoming,     // a remote peer connected to our listen socket (already accepted)
    Connected,    // an outgoing connection (or loopback pair end) is ready
    Disconnected, // closed by the peer or by a local problem (timeout, etc.)
};

struct ConnEvent
{
    ConnEventType type{};
    ConnId conn = kInvalidConn;
    std::string reason;
};

struct Packet
{
    ConnId conn = kInvalidConn;
    Lane lane = Lane::Control;
    std::vector<uint8_t> data;
};

struct ConnStats
{
    int pingMs = -1;
    float qualityLocal = -1.0f; // fraction of packets delivered, -1 unknown
    float outBytesPerSec = 0.0f;
    float inBytesPerSec = 0.0f;
};

// Message transport with lanes (docs/01-architecture.md §5). One instance owns its own connections;
// several instances may live in the same process (tests, sim tools, the host's own client).
class ITransport
{
public:
    virtual ~ITransport() = default;

    virtual bool Listen(uint16_t aPort, std::string& aError) = 0;
    virtual ConnId Connect(const std::string& aAddress, std::string& aError) = 0;
    virtual bool Send(ConnId aConn, Lane aLane, const void* aData, size_t aSize, bool aReliable) = 0;
    virtual void Close(ConnId aConn, int aReason, const char* aDebug) = 0;

    // Drains connection events and received packets since the last call.
    virtual void Poll(std::vector<ConnEvent>& aEvents, std::vector<Packet>& aPackets) = 0;

    virtual ConnStats Stats(ConnId aConn) const = 0;

    bool Send(ConnId aConn, Lane aLane, const std::vector<uint8_t>& aData, bool aReliable)
    {
        return Send(aConn, aLane, aData.data(), aData.size(), aReliable);
    }
};
} // namespace coop
