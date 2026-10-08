#pragma once

#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "net/Transport.hpp"

namespace coop
{
// Network impairment presets (docs/05-local-testing.md §4). Applied process-wide, because
// GameNetworkingSockets' fake lag/loss settings are global.
enum class ImpairmentPreset
{
    None,
    Lan,
    GoodHome,
    Typical,
    Bad,
    Awful,
};

bool ParseImpairmentPreset(const std::string& aName, ImpairmentPreset& aOut);
const char* ToString(ImpairmentPreset aPreset);

// Open-source GameNetworkingSockets backend.
class GnsTransport final : public ITransport
{
public:
    GnsTransport();
    ~GnsTransport() override;

    GnsTransport(const GnsTransport&) = delete;
    GnsTransport& operator=(const GnsTransport&) = delete;

    // Must succeed before any instance is used. Reference-counted; safe to call repeatedly.
    static bool InitLibrary(std::string& aError);
    // aKill=false leaves the library loaded (useful inside the game process at shutdown).
    static void ShutdownLibrary(bool aKill = true);
    static void ApplyImpairment(ImpairmentPreset aPreset);

    // In-process connection between two transports (the host's own client uses this).
    static bool CreatePair(GnsTransport& aFirst, ConnId& aFirstConn, GnsTransport& aSecond, ConnId& aSecondConn);

    bool Listen(uint16_t aPort, std::string& aError) override;
    ConnId Connect(const std::string& aAddress, std::string& aError) override;
    bool Send(ConnId aConn, Lane aLane, const void* aData, size_t aSize, bool aReliable) override;
    void Close(ConnId aConn, int aReason, const char* aDebug) override;
    void Poll(std::vector<ConnEvent>& aEvents, std::vector<Packet>& aPackets) override;
    ConnStats Stats(ConnId aConn) const override;

    using ITransport::Send;

    // Called from the library callback (routed through a process-wide registry).
    void OnStatusChanged(uint32_t aConn, uint32_t aListenSocket, int aNewState, int aEndReason,
                         const char* aDebug);

private:
    void Adopt(ConnId aConn, bool aAnnounceConnected);
    void Forget(ConnId aConn);
    void AnnounceConnected(ConnId aConn);
    void ConfigureLanes(ConnId aConn);
    bool Has(ConnId aConn) const;

    uint32_t m_listenSocket = 0;
    uint32_t m_pollGroup = 0;

    // Status callbacks can arrive on another session's thread (see the registry in GnsTransport.cpp).
    mutable std::mutex m_stateMutex;
    std::set<ConnId> m_connections;
    std::set<ConnId> m_announced;

    std::mutex m_eventMutex;
    std::deque<ConnEvent> m_events;
};
} // namespace coop
