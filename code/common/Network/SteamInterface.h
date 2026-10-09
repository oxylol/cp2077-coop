#pragma once

#include "steam/steamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"


enum EPacketFlags
{
    kReliable,
    kUnreliable
};

enum EConnectOpcode : uint8_t
{
    kPayload = 0,
    kServerTime = 1,
    kCompressedPayload = 2,
    kHandshake = 3
};

struct SteamInterface
{
    static void Acquire();
    static void Release();

    // The ISteamNetworkingSockets that new servers and connections use: this library's (GameNetworkingSockets)
    // unless the Steam client's was set, for relayed connections between Steam users (SteamID, no address). Both
    // implement "SteamNetworkingSockets012", the interface this library's headers declare.
    static ISteamNetworkingSockets* Sockets() noexcept;
    // The configuration of the same implementation as Sockets().
    static ISteamNetworkingUtils* Utils() noexcept;
    // Both from the same implementation, or nullptr for this library's again.
    static void SetSockets(ISteamNetworkingSockets* apSockets, ISteamNetworkingUtils* apUtils) noexcept;
    // Utils() of the implementation apSockets belongs to.
    static ISteamNetworkingUtils* UtilsFor(const ISteamNetworkingSockets* apSockets) noexcept;
};

using ConnectionId = HSteamNetConnection;

