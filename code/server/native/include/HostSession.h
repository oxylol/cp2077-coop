#pragma once

#include <cstdint>
#include <string>

#include <steam/steamnetworkingsockets.h>

// A co-op session run inside this game (a "listen server"): the host's game runs it and joins it like everyone
// else, through a connection within the process (OpenLocalConnection). Call everything from the game's main thread.
namespace HostSession
{
struct Settings
{
    // The first port tried; the next ones are tried when it's taken (e.g. by another game on this PC).
    uint16_t Port = 11778;
    uint16_t MaxPlayers = 4;
    // Every guest has to send this; empty means no password.
    std::string Password;
    // The host's own game authenticates with this, which makes it the story host. Random per session.
    std::string HostToken;
    // Also reachable through Steam (Client::ConnectP2P to the host's SteamID): set the Steam client's sockets first
    // (SteamInterface::SetSockets).
    bool SteamP2P = false;
};

// False when no port could be opened.
bool Start(const Settings& acSettings);
// Ends the session: every player is disconnected.
void Stop();
// Once per frame while running.
void Update();
bool IsRunning();
// The port actually listened on.
uint16_t GetPort();
// Steam users can connect (Settings::SteamP2P, and it worked).
bool IsReachableThroughSteam();

// The host's own game's connection to the session, within this process: give both to Client::Adopt(). False when
// the session isn't running.
bool OpenLocalConnection(ISteamNetworkingSockets*& apSockets, HSteamNetConnection& aConnection);
} // namespace HostSession
