#pragma once

#include <steam/steamnetworkingsockets.h>
#include <cstdint>
#include <chrono>
#include "SteamInterface.h"
#include <Core/Stl.h>


struct Packet;
struct Server
{
    Server(uint64_t aClientIdentifier, uint64_t aServerIdentifier) noexcept;
    virtual ~Server();

    TP_NOCOPYMOVE(Server);

    bool Host(uint16_t aPort, uint32_t aTickRate, bool bEnableDualStackIP = true) noexcept;
    // After Host(): also accepts Steam users connecting to this Steam user (Client::ConnectP2P), relayed by Steam.
    // Needs the Steam client's sockets (SteamInterface).
    bool HostP2P() noexcept;
    // A connection to this server within this process (the host's own game): the server keeps one end, the other
    // goes to Client::Adopt() with GetSockets(). No network involved.
    HSteamNetConnection OpenLocalConnection() noexcept;
    [[nodiscard]] ISteamNetworkingSockets* GetSockets() const noexcept { return m_pInterface; }
    void Close() noexcept;
    // Closes every connection but keeps listening. The close notices go out through the listen socket's UDP port,
    // so keep it open (and updated) a little longer if the players should learn about it rather than time out.
    void CloseConnections(const char* acReason) noexcept;

    void Update() noexcept;
    // Blocking (the default): Update() sleeps to keep the tick rate, for a server with a thread of its own. A server
    // updated from someone else's loop (the game's frame) sets this to false.
    void SetBlocking(bool aBlocking) noexcept { m_blocking = aBlocking; }

    enum EDisconnectReason : int
    {
        Unknown,
        Quit,
        Kicked,
        Banned,
        BadConnection,
        TimedOut
    };

    virtual void OnUpdate() = 0;
    virtual void OnConsume(const void* apData, uint32_t aSize, ConnectionId aConnectionId) = 0;
    virtual void OnConnection(ConnectionId aHandle) = 0;
    virtual void OnDisconnection(ConnectionId aConnectionId,
                                 EDisconnectReason aReason) = 0;

    void SendToAll(Packet* apPacket, EPacketFlags aPacketFlags = kReliable) noexcept;
    void Send(ConnectionId aConnectionId, Packet* apPacket, EPacketFlags aPacketFlags = kReliable) const noexcept;
    void Kick(ConnectionId aConnectionId) noexcept;

    [[nodiscard]] uint16_t GetPort() const noexcept;
    [[nodiscard]] bool IsListening() const noexcept;
    [[nodiscard]] bool IsListeningP2P() const noexcept;
    [[nodiscard]] uint32_t GetClientCount() const noexcept;
    [[nodiscard]] uint32_t GetTickRate() const noexcept;
    [[nodiscard]] uint64_t GetTick() const noexcept;
    [[nodiscard]] SteamNetConnectionInfo_t GetConnectionInfo(ConnectionId aConnectionId) const noexcept;
    [[nodiscard]] bool IsAlive(ConnectionId aConnectionId) const noexcept;

private:

    void Remove(ConnectionId aId) noexcept;

    void HandleMessage(const void* apData, uint32_t aSize, ConnectionId aConnectionId) noexcept;
    void HandleCompressedPayload(const void* apData, uint32_t aSize, ConnectionId aConnectionId) noexcept;
    void HandleHandshake(const uint8_t* apData, uint32_t aSize, ConnectionId aConnectionId) noexcept;

    void SynchronizeClientClocks(ConnectionId aSpecificConnection = k_HSteamNetConnection_Invalid) noexcept;

    static void SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* apInfo);
    void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* apInfo);

    HSteamListenSocket m_listenSock;
    HSteamListenSocket m_p2pListenSock = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup m_pollGroup;
    ISteamNetworkingSockets* m_pInterface;

    Vector<ConnectionId> m_queuedConnections;
    Vector<ConnectionId> m_connections;

    uint32_t m_tickRate;
    std::chrono::time_point<std::chrono::high_resolution_clock> m_lastClockSyncTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> m_lastUpdateTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> m_currentTick;
    std::chrono::milliseconds m_timeBetweenUpdates;

    uint64_t m_clientIdentifier;
    uint64_t m_serverIdentifier;
    bool m_blocking{true};
};

