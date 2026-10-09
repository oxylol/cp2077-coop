#pragma once

namespace fs = std::filesystem;

// The co-op settings: coop.ini next to the plugin (created with comments on first start), each overridable on the
// game's command line (--name=..., --password=..., --port=..., --join=...), e.g. for two games on one PC.
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
    // The port a hosted session listens on (the next ones are tried when it's taken).
    uint16_t port = 11778;
    // Where Join connects: the host's address and port.
    String joinAddress = "127.0.0.1:11778";
    // Players in a hosted session, the host included.
    uint16_t maxPlayers = 4;

    fs::path iniPath{};

private:
    Settings() = default;
};
