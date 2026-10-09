#pragma once

#include <cstdint>
#include <string>

// A co-op session run inside this game (a "listen server"): the host's game runs it and joins it like everyone
// else, through the loopback address. Call everything from the game's main thread.
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
} // namespace HostSession
