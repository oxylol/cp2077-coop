#pragma once

class ISteamNetworkingSockets;
class ISteamNetworkingUtils;

// The game's Steam client, for sessions between Steam users without addresses or port forwarding: its networking
// sockets carry the session (relayed by Steam), and a lobby tagged with a hash of the password lets the guests find
// the host. Everything is looked up at run time in the game's steam_api64.dll: without Steam (GOG, Epic, Steam not
// running) or with an unexpected Steam API, Init() fails with the reason and the mod connects by address only.
// Main thread only.
struct SteamLobby
{
    // Finds Steam's interfaces once Steam is up (after the main menu): true when it's usable. aWhy gets the reason
    // when not.
    static bool Init(std::string& aWhy);
    // Steam's networking sockets and their configuration, for SteamInterface::SetSockets (nullptr before a
    // successful Init()).
    static ISteamNetworkingSockets* GetSockets();
    static ISteamNetworkingUtils* GetNetworkingUtils();
    // This game's Steam user, or 0.
    static uint64_t GetSteamId();
    // What the lobby is tagged with for a password: guests with the same password find the host's lobby.
    static std::string KeyFor(const std::string& acPassword);

    // Host: opens a lobby with this key that guests find (Find). The outcome comes from Update().
    void Create(const std::string& acKey, const std::string& acHostName, uint32_t aMaxPlayers);
    // Guest: looks for the lobby with this key. The outcome (the host's SteamID) comes from Update().
    void Find(const std::string& acKey);
    // Leaves the lobby (host) or stops looking (guest).
    void Leave();
    bool IsBusy() const noexcept { return m_state == State::kCreating || m_state == State::kFinding; }

    struct Result
    {
        bool Ok = false;
        // Find: the host's SteamID and name, and how many lobbies had the key (more than one: same password).
        uint64_t HostSteamId = 0;
        std::string HostName;
        uint32_t Matches = 0;
        std::string Error;
    };
    // Once per frame: the outcome of Create or Find, once.
    std::optional<Result> Update();

private:
    enum class State
    {
        kIdle,
        kCreating,
        kInLobby,
        kFinding
    };

    State m_state = State::kIdle;
    uint64_t m_call = 0;  // SteamAPICall_t
    uint64_t m_lobby = 0; // the host's lobby
    std::string m_key;
    std::string m_hostName;
    std::chrono::steady_clock::time_point m_deadline{};
    // Lobbies still being created when they were no longer wanted: left once they exist.
    std::vector<uint64_t> m_abandoned;
};
