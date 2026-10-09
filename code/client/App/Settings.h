#pragma once

namespace fs = std::filesystem;

// The co-op settings: coop.ini in the plugin folder (code/assets/coop.ini, written there when missing), each
// overridable on the game's command line (--name=..., --password=..., --join=..., --steam=false, --port=...), e.g.
// for two games on one PC.
struct Settings
{
    static Settings& Get()
    {
        static Settings instance;
        return instance;
    }
    static void Load();

    // Shown to the other players.
    String name{};
    // Host and guests need the same one; empty means none.
    String password{};
    // What coop.ini comes with: not good enough to find each other through Steam.
    static constexpr auto kDefaultPassword = "changeme";
    // Sessions between Steam users go through Steam (relayed, found by the password). False: addresses only.
    bool steam = true;
    // The port a hosted session listens on (the next ones are tried when it's taken).
    uint16_t port = 11778;
    // Where Join connects: the host's address and port. Empty: the host is found through Steam by the password.
    String joinAddress{};
    // Players in a hosted session, the host included.
    uint16_t maxPlayers = 4;

    fs::path iniPath{};

private:
    Settings() = default;
};
