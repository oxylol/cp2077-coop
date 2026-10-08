#pragma once

// In-memory ITransport for deterministic tests: no sockets, no threads, instant delivery in send order.
// Pair it with ManualClock to step a whole session frame by frame.

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "net/Transport.hpp"

namespace test
{
class MemoryTransport;

class MemoryNetwork
{
public:
    std::map<uint16_t, MemoryTransport*> listeners;
    coop::ConnId nextConn = 1;
};

class MemoryTransport final : public coop::ITransport
{
public:
    explicit MemoryTransport(MemoryNetwork& aNetwork)
        : m_network(aNetwork)
    {
    }

    ~MemoryTransport() override
    {
        for (auto& [conn, link] : std::map<coop::ConnId, Link>(m_links))
            Close(conn, 0, "transport destroyed");
        for (auto it = m_network.listeners.begin(); it != m_network.listeners.end();)
            it = it->second == this ? m_network.listeners.erase(it) : std::next(it);
    }

    // Connects two transports directly (like GnsTransport::CreatePair); both ends get Connected.
    static void CreatePair(MemoryTransport& aA, coop::ConnId& aConnA, MemoryTransport& aB, coop::ConnId& aConnB)
    {
        aConnA = aA.m_network.nextConn++;
        aConnB = aA.m_network.nextConn++;
        aA.m_links[aConnA] = {&aB, aConnB};
        aB.m_links[aConnB] = {&aA, aConnA};
        aA.m_events.push_back({coop::ConnEventType::Connected, aConnA, {}});
        aB.m_events.push_back({coop::ConnEventType::Connected, aConnB, {}});
    }

    bool Listen(uint16_t aPort, std::string& aError) override
    {
        if (m_network.listeners.count(aPort))
        {
            aError = "port in use";
            return false;
        }
        m_network.listeners[aPort] = this;
        return true;
    }

    // Address "anything:port"; only the port matters.
    coop::ConnId Connect(const std::string& aAddress, std::string& aError) override
    {
        const auto colon = aAddress.rfind(':');
        const auto port = static_cast<uint16_t>(std::stoi(aAddress.substr(colon + 1)));
        auto it = m_network.listeners.find(port);
        if (it == m_network.listeners.end())
        {
            aError = "nobody listening";
            return coop::kInvalidConn;
        }
        MemoryTransport& listener = *it->second;
        const coop::ConnId mine = m_network.nextConn++;
        const coop::ConnId theirs = m_network.nextConn++;
        m_links[mine] = {&listener, theirs};
        listener.m_links[theirs] = {this, mine};
        listener.m_events.push_back({coop::ConnEventType::Incoming, theirs, {}});
        m_events.push_back({coop::ConnEventType::Connected, mine, {}});
        return mine;
    }

    bool Send(coop::ConnId aConn, coop::Lane aLane, const void* aData, size_t aSize, bool aReliable) override
    {
        auto it = m_links.find(aConn);
        if (it == m_links.end())
            return false;
        if (!aReliable && dropUnreliable)
            return true;
        const auto* bytes = static_cast<const uint8_t*>(aData);
        it->second.peer->m_packets.push_back({it->second.peerConn, aLane, std::vector<uint8_t>(bytes, bytes + aSize)});
        ++sent;
        return true;
    }

    using coop::ITransport::Send;

    void Close(coop::ConnId aConn, int, const char* aDebug) override
    {
        auto it = m_links.find(aConn);
        if (it == m_links.end())
            return;
        const Link link = it->second;
        m_links.erase(it);
        if (link.peer->m_links.erase(link.peerConn) > 0)
            link.peer->m_events.push_back({coop::ConnEventType::Disconnected, link.peerConn, aDebug ? aDebug : ""});
    }

    void Poll(std::vector<coop::ConnEvent>& aEvents, std::vector<coop::Packet>& aPackets) override
    {
        while (!m_events.empty())
        {
            aEvents.push_back(std::move(m_events.front()));
            m_events.pop_front();
        }
        while (!m_packets.empty())
        {
            aPackets.push_back(std::move(m_packets.front()));
            m_packets.pop_front();
        }
    }

    coop::ConnStats Stats(coop::ConnId) const override
    {
        coop::ConnStats stats;
        stats.pingMs = 0;
        return stats;
    }

    // The only connection of a client transport (for injecting raw messages in tests).
    [[nodiscard]] coop::ConnId FirstConnection() const { return m_links.empty() ? coop::kInvalidConn : m_links.begin()->first; }

    bool dropUnreliable = false;
    uint64_t sent = 0;

private:
    struct Link
    {
        MemoryTransport* peer = nullptr;
        coop::ConnId peerConn = coop::kInvalidConn;
    };

    MemoryNetwork& m_network;
    std::map<coop::ConnId, Link> m_links;
    std::deque<coop::ConnEvent> m_events;
    std::deque<coop::Packet> m_packets;
};
} // namespace test
