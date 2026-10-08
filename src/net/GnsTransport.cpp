#include "net/GnsTransport.hpp"

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>

#include "core/Log.hpp"

namespace coop
{
namespace
{
// Process-wide routing from library callbacks to the owning transport instance.
//
// The library delivers status callbacks to whichever thread calls RunCallbacks, for every connection in the
// process. With several sessions in one process (tests, tools) that can be another transport's thread, so the
// registry lock is held while a callback runs: a transport can't be destroyed mid-callback. The library
// releases its own locks before dispatching, and callbacks may call back into the registry, hence recursive.
struct Registry
{
    std::recursive_mutex mutex;
    int refCount = 0;
    bool alive = false; // the library stays alive after ShutdownLibrary(false)
    std::map<HSteamNetConnection, GnsTransport*> connections;
    std::map<HSteamListenSocket, GnsTransport*> listenSockets;
};

Registry& GetRegistry()
{
    static Registry s_registry;
    return s_registry;
}

void OnStatusChangedStatic(SteamNetConnectionStatusChangedCallback_t* aInfo)
{
    auto& registry = GetRegistry();
    std::scoped_lock lock(registry.mutex);

    GnsTransport* owner = nullptr;
    auto it = registry.connections.find(aInfo->m_hConn);
    if (it != registry.connections.end())
    {
        owner = it->second;
    }
    else if (aInfo->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid)
    {
        auto listenIt = registry.listenSockets.find(aInfo->m_info.m_hListenSocket);
        if (listenIt != registry.listenSockets.end())
            owner = listenIt->second;
    }

    if (!owner)
        return;

    owner->OnStatusChanged(aInfo->m_hConn, aInfo->m_info.m_hListenSocket, aInfo->m_info.m_eState,
                           aInfo->m_info.m_eEndReason, aInfo->m_info.m_szEndDebug);
}

void OnDebugOutput(ESteamNetworkingSocketsDebugOutputType aType, const char* aMessage)
{
    const LogLevel level = aType <= k_ESteamNetworkingSocketsDebugOutputType_Error ? LogLevel::Error
                         : aType <= k_ESteamNetworkingSocketsDebugOutputType_Warning ? LogLevel::Warn
                                                                                       : LogLevel::Debug;
    Log::Format(level, "[gns] %s", aMessage);
}

ISteamNetworkingSockets* Sockets()
{
    return SteamNetworkingSockets();
}

ISteamNetworkingUtils* Utils()
{
    return SteamNetworkingUtils();
}
} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Library lifetime

bool GnsTransport::InitLibrary(std::string& aError)
{
    auto& registry = GetRegistry();
    std::scoped_lock lock(registry.mutex);
    if (registry.refCount++ > 0 || registry.alive)
        return true;

    SteamNetworkingErrMsg error{};
    if (!GameNetworkingSockets_Init(nullptr, error))
    {
        --registry.refCount;
        aError = error;
        return false;
    }
    registry.alive = true;

    Utils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Warning, &OnDebugOutput);
    Utils()->SetGlobalCallback_SteamNetConnectionStatusChanged(&OnStatusChangedStatic);
    return true;
}

void GnsTransport::ShutdownLibrary(bool aKill)
{
    auto& registry = GetRegistry();
    std::scoped_lock lock(registry.mutex);
    if (registry.refCount == 0)
        return;
    if (--registry.refCount == 0 && aKill && registry.alive)
    {
        GameNetworkingSockets_Kill();
        registry.alive = false;
    }
}

void GnsTransport::ApplyImpairment(ImpairmentPreset aPreset)
{
    struct Values
    {
        int lagMs;        // per direction
        float jitterAvgMs;
        float jitterMaxMs;
        float lossPct;    // per direction
        int rateBytesPerSec;
    };

    Values v{};
    switch (aPreset)
    {
    case ImpairmentPreset::None: v = {0, 0.0f, 0.0f, 0.0f, 0}; break;
    case ImpairmentPreset::Lan: v = {1, 0.0f, 0.0f, 0.0f, 0}; break;
    case ImpairmentPreset::GoodHome: v = {20, 5.0f, 15.0f, 0.5f, 0}; break;
    case ImpairmentPreset::Typical: v = {60, 15.0f, 45.0f, 1.0f, 5'000'000 / 8}; break;
    case ImpairmentPreset::Bad: v = {100, 30.0f, 90.0f, 3.0f, 2'000'000 / 8}; break;
    case ImpairmentPreset::Awful: v = {150, 60.0f, 180.0f, 5.0f, 1'000'000 / 8}; break;
    }

    auto* utils = Utils();
    utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, v.lagMs);
    utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Recv, v.lagMs);
    utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, v.lossPct);
    utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, v.lossPct);
    utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Avg, v.jitterAvgMs);
    utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Max, v.jitterMaxMs);
    utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Pct,
                                     v.jitterAvgMs > 0.0f ? 50.0f : 0.0f);
    utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakeRateLimit_Send_Rate, v.rateBytesPerSec);
    utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakeRateLimit_Send_Burst,
                                     v.rateBytesPerSec > 0 ? v.rateBytesPerSec / 10 : 0);

    COOP_LOG_INFO("network impairment: %s", ToString(aPreset));
}

bool GnsTransport::CreatePair(GnsTransport& aFirst, ConnId& aFirstConn, GnsTransport& aSecond, ConnId& aSecondConn)
{
    HSteamNetConnection first = k_HSteamNetConnection_Invalid;
    HSteamNetConnection second = k_HSteamNetConnection_Invalid;
    if (!Sockets()->CreateSocketPair(&first, &second, false, nullptr, nullptr))
        return false;

    aFirst.Adopt(first, true);
    aSecond.Adopt(second, true);
    aFirstConn = first;
    aSecondConn = second;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Instance

GnsTransport::GnsTransport()
{
    m_pollGroup = Sockets()->CreatePollGroup();
}

GnsTransport::~GnsTransport()
{
    // Stop routing new incoming connections to this transport first, so none gets adopted while closing.
    if (m_listenSocket != k_HSteamListenSocket_Invalid)
    {
        auto& registry = GetRegistry();
        std::scoped_lock lock(registry.mutex);
        registry.listenSockets.erase(m_listenSocket);
    }

    // Close the existing connections gracefully (lingering, so a final Kick still arrives)...
    std::set<ConnId> connections;
    {
        std::scoped_lock lock(m_stateMutex);
        connections = m_connections;
    }
    for (const auto conn : connections)
        Close(conn, 0, "transport destroyed");

    // ...and only then the listen socket, which would drop its connections without a goodbye.
    if (m_listenSocket != k_HSteamListenSocket_Invalid)
        Sockets()->CloseListenSocket(m_listenSocket);

    if (m_pollGroup != k_HSteamNetPollGroup_Invalid)
        Sockets()->DestroyPollGroup(m_pollGroup);
}

bool GnsTransport::Listen(uint16_t aPort, std::string& aError)
{
    if (m_listenSocket != k_HSteamListenSocket_Invalid)
    {
        aError = "already listening";
        return false;
    }

    SteamNetworkingIPAddr address;
    address.Clear();
    address.m_port = aPort;

    const HSteamListenSocket socket = Sockets()->CreateListenSocketIP(address, 0, nullptr);
    if (socket == k_HSteamListenSocket_Invalid)
    {
        aError = "could not listen on UDP port " + std::to_string(aPort);
        return false;
    }

    {
        auto& registry = GetRegistry();
        std::scoped_lock lock(registry.mutex);
        registry.listenSockets[socket] = this;
    }
    m_listenSocket = socket;
    return true;
}

ConnId GnsTransport::Connect(const std::string& aAddress, std::string& aError)
{
    SteamNetworkingIPAddr address;
    address.Clear();
    if (!address.ParseString(aAddress.c_str()))
    {
        aError = "invalid address '" + aAddress + "' (expected ip:port)";
        return kInvalidConn;
    }
    if (address.m_port == 0)
        address.m_port = kDefaultPort;

    const HSteamNetConnection conn = Sockets()->ConnectByIPAddress(address, 0, nullptr);
    if (conn == k_HSteamNetConnection_Invalid)
    {
        aError = "could not start connection to " + aAddress;
        return kInvalidConn;
    }

    Adopt(conn, false);
    return conn;
}

void GnsTransport::Adopt(ConnId aConn, bool aAnnounceConnected)
{
    {
        auto& registry = GetRegistry();
        std::scoped_lock lock(registry.mutex);
        registry.connections[aConn] = this;
    }
    {
        std::scoped_lock lock(m_stateMutex);
        m_connections.insert(aConn);
    }
    Sockets()->SetConnectionPollGroup(aConn, m_pollGroup);
    ConfigureLanes(aConn);

    if (aAnnounceConnected)
        AnnounceConnected(aConn);
}

void GnsTransport::AnnounceConnected(ConnId aConn)
{
    {
        std::scoped_lock lock(m_stateMutex);
        if (!m_announced.insert(aConn).second)
            return; // socket pairs may also report Connected through the callback
    }
    std::scoped_lock lock(m_eventMutex);
    m_events.push_back({ConnEventType::Connected, aConn, {}});
}

void GnsTransport::Forget(ConnId aConn)
{
    {
        auto& registry = GetRegistry();
        std::scoped_lock lock(registry.mutex);
        registry.connections.erase(aConn);
    }
    std::scoped_lock lock(m_stateMutex);
    m_connections.erase(aConn);
    m_announced.erase(aConn);
}

bool GnsTransport::Has(ConnId aConn) const
{
    std::scoped_lock lock(m_stateMutex);
    return m_connections.find(aConn) != m_connections.end();
}

void GnsTransport::ConfigureLanes(ConnId aConn)
{
    // Control first; events and state share priority 1 by weight; bulk and diagnostics last.
    static constexpr int kPriorities[static_cast<int>(Lane::Count)] = {0, 1, 1, 2, 3};
    static constexpr uint16 kWeights[static_cast<int>(Lane::Count)] = {1, 60, 40, 1, 1};
    const EResult result =
        Sockets()->ConfigureConnectionLanes(aConn, static_cast<int>(Lane::Count), kPriorities, kWeights);
    if (result != k_EResultOK)
        COOP_LOG_WARN("could not configure lanes on connection %u (result %d)", aConn, static_cast<int>(result));
}

void GnsTransport::OnStatusChanged(uint32_t aConn, uint32_t aListenSocket, int aNewState, int aEndReason,
                                   const char* aDebug)
{
    switch (aNewState)
    {
    case k_ESteamNetworkingConnectionState_Connecting:
    {
        // Incoming connections show up here first; outgoing ones are already registered.
        if (aListenSocket == k_HSteamListenSocket_Invalid || Has(aConn))
            break;

        if (Sockets()->AcceptConnection(aConn) != k_EResultOK)
        {
            Sockets()->CloseConnection(aConn, 0, "accept failed", false);
            break;
        }
        Adopt(aConn, false);
        std::scoped_lock lock(m_eventMutex);
        m_events.push_back({ConnEventType::Incoming, aConn, {}});
        break;
    }

    case k_ESteamNetworkingConnectionState_Connected:
    {
        // Incoming connections were already announced as Incoming.
        if (aListenSocket != k_HSteamListenSocket_Invalid || !Has(aConn))
            break;
        AnnounceConnected(aConn);
        break;
    }

    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
    {
        if (!Has(aConn))
            break;
        {
            std::scoped_lock lock(m_eventMutex);
            std::string reason = aDebug ? aDebug : "";
            if (reason.empty())
                reason = "connection closed (" + std::to_string(aEndReason) + ")";
            m_events.push_back({ConnEventType::Disconnected, aConn, std::move(reason)});
        }
        Sockets()->CloseConnection(aConn, 0, nullptr, false);
        Forget(aConn);
        break;
    }

    default:
        break;
    }
}

bool GnsTransport::Send(ConnId aConn, Lane aLane, const void* aData, size_t aSize, bool aReliable)
{
    if (aSize == 0 || !Has(aConn))
        return false;

    SteamNetworkingMessage_t* message = Utils()->AllocateMessage(static_cast<int>(aSize));
    if (!message)
        return false;
    std::memcpy(message->m_pData, aData, aSize);
    message->m_conn = aConn;
    message->m_nFlags = aReliable ? k_nSteamNetworkingSend_ReliableNoNagle : k_nSteamNetworkingSend_UnreliableNoNagle;
    message->m_idxLane = static_cast<uint16>(aLane);

    int64 result = 0;
    Sockets()->SendMessages(1, &message, &result, true);
    return result >= 0;
}

void GnsTransport::Close(ConnId aConn, int aReason, const char* aDebug)
{
    if (!Has(aConn))
        return;
    // Application reason codes must fall in the library's app range.
    const int reason = std::clamp(k_ESteamNetConnectionEnd_App_Min + aReason, static_cast<int>(k_ESteamNetConnectionEnd_App_Min),
                                  static_cast<int>(k_ESteamNetConnectionEnd_App_Max));
    // Linger so queued reliable messages (e.g. a Reject) still reach the peer.
    Sockets()->CloseConnection(aConn, reason, aDebug, true);
    Forget(aConn);
}

void GnsTransport::Poll(std::vector<ConnEvent>& aEvents, std::vector<Packet>& aPackets)
{
    Sockets()->RunCallbacks();

    {
        std::scoped_lock lock(m_eventMutex);
        while (!m_events.empty())
        {
            aEvents.push_back(std::move(m_events.front()));
            m_events.pop_front();
        }
    }

    SteamNetworkingMessage_t* messages[64];
    for (;;)
    {
        const int count = Sockets()->ReceiveMessagesOnPollGroup(m_pollGroup, messages, 64);
        if (count <= 0)
            break;

        for (int i = 0; i < count; ++i)
        {
            auto* message = messages[i];
            Packet packet;
            packet.conn = message->m_conn;
            packet.lane = static_cast<Lane>(std::min<int>(message->m_idxLane, static_cast<int>(Lane::Count) - 1));
            const auto* bytes = static_cast<const uint8_t*>(message->m_pData);
            packet.data.assign(bytes, bytes + message->m_cbSize);
            aPackets.push_back(std::move(packet));
            message->Release();
        }
    }
}

ConnStats GnsTransport::Stats(ConnId aConn) const
{
    ConnStats stats;
    SteamNetConnectionRealTimeStatus_t status{};
    if (Sockets()->GetConnectionRealTimeStatus(aConn, &status, 0, nullptr) == k_EResultOK)
    {
        stats.pingMs = status.m_nPing;
        stats.qualityLocal = status.m_flConnectionQualityLocal;
        stats.outBytesPerSec = status.m_flOutBytesPerSec;
        stats.inBytesPerSec = status.m_flInBytesPerSec;
    }
    return stats;
}

// ---------------------------------------------------------------------------------------------------------------------
// Presets

bool ParseImpairmentPreset(const std::string& aName, ImpairmentPreset& aOut)
{
    std::string name = aName;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name == "none" || name.empty()) aOut = ImpairmentPreset::None;
    else if (name == "lan") aOut = ImpairmentPreset::Lan;
    else if (name == "good" || name == "goodhome") aOut = ImpairmentPreset::GoodHome;
    else if (name == "typical") aOut = ImpairmentPreset::Typical;
    else if (name == "bad") aOut = ImpairmentPreset::Bad;
    else if (name == "awful") aOut = ImpairmentPreset::Awful;
    else return false;
    return true;
}

const char* ToString(ImpairmentPreset aPreset)
{
    switch (aPreset)
    {
    case ImpairmentPreset::None: return "none";
    case ImpairmentPreset::Lan: return "lan";
    case ImpairmentPreset::GoodHome: return "good";
    case ImpairmentPreset::Typical: return "typical";
    case ImpairmentPreset::Bad: return "bad";
    case ImpairmentPreset::Awful: return "awful";
    }
    return "?";
}
} // namespace coop
