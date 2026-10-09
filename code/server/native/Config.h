#pragma once

// Settings of a co-op session hosted by this game (see HostSession.h).
struct Config
{
    uint16_t Port{11778};
    uint16_t MaxPlayer{4};
    uint16_t TickRate{60};
    uint16_t UpdateRate{10};
    // Every player has to send this; empty means no password.
    std::string Password{};
    // The host's own game authenticates with this (random per session), which makes it the story host.
    std::string HostToken{};
    // Also accept Steam users connecting to the host's Steam user (needs the Steam client's sockets).
    bool SteamP2P{false};
};
