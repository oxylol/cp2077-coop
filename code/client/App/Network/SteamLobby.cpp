#include "SteamLobby.h"

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include <Windows.h>
#include <bcrypt.h>

namespace
{
// The parts of the Steamworks API used here, as steam_api_flat.h declares them. The game's steam_api64.dll exports
// them; their values and layouts are fixed by Steam.
using HSteamUserHandle = int32_t;
using SteamAPICall = uint64_t;
struct Matchmaking; // ISteamMatchmaking
struct Utils;       // ISteamUtils

constexpr int kLobbyTypePublic = 2;        // ELobbyType: found by searches
constexpr int kLobbyComparisonEqual = 0;   // ELobbyComparison
constexpr int kLobbyDistanceWorldwide = 3; // ELobbyDistanceFilter
constexpr int32_t kResultOK = 1;           // EResult

// Call results (isteammatchmaking.h), laid out as Steam does on Windows (8-byte packing).
struct LobbyCreated
{
    static constexpr int kCallback = 513; // k_iSteamMatchmakingCallbacks + 13
    int32_t Result;                       // EResult
    uint64_t SteamIDLobby;
};
static_assert(sizeof(LobbyCreated) == 16);

struct LobbyMatchList
{
    static constexpr int kCallback = 510; // k_iSteamMatchmakingCallbacks + 10
    uint32_t LobbiesMatching;
};

struct Api
{
    HSteamUserHandle (*GetHSteamUser)() = nullptr;
    void* (*FindOrCreateUserInterface)(HSteamUserHandle, const char*) = nullptr;
    Matchmaking* (*GetMatchmaking)() = nullptr;
    SteamAPICall (*CreateLobby)(Matchmaking*, int, int) = nullptr;
    bool (*SetLobbyData)(Matchmaking*, uint64_t, const char*, const char*) = nullptr;
    void (*LeaveLobby)(Matchmaking*, uint64_t) = nullptr;
    void (*AddRequestLobbyListStringFilter)(Matchmaking*, const char*, const char*, int) = nullptr;
    void (*AddRequestLobbyListDistanceFilter)(Matchmaking*, int) = nullptr;
    void (*AddRequestLobbyListResultCountFilter)(Matchmaking*, int) = nullptr;
    SteamAPICall (*RequestLobbyList)(Matchmaking*) = nullptr;
    uint64_t (*GetLobbyByIndex)(Matchmaking*, int) = nullptr;
    const char* (*GetLobbyData)(Matchmaking*, uint64_t, const char*) = nullptr;
    Utils* (*GetUtils)() = nullptr;
    bool (*IsAPICallCompleted)(Utils*, SteamAPICall, bool*) = nullptr;
    bool (*GetAPICallResult)(Utils*, SteamAPICall, void*, int, int, bool*) = nullptr;
};

// The lobby's fields: the key guests search for, and who hosts.
constexpr auto kKeyField = "cp2077coop_key";
constexpr auto kHostIdField = "cp2077coop_host";
constexpr auto kHostNameField = "cp2077coop_name";

constexpr auto kCreateTimeout = std::chrono::seconds(20);
constexpr auto kFindTimeout = std::chrono::seconds(15);

// steam_api64.dll's exports, looked up once. nullptr, with the reason, when the game has no Steam API (GOG, Epic)
// or lacks a part of it.
const Api* LoadApi(std::string& aWhy)
{
    static Api s_api;
    static std::string s_error;
    static bool s_loaded = false;

    if (!s_loaded)
    {
        s_loaded = true;

        const auto module = GetModuleHandleW(L"steam_api64.dll");
        if (!module)
        {
            s_error = "this copy of the game doesn't run on Steam";
        }
        else
        {
            std::string missing;
            const auto resolve = [&](auto& aFunction, std::initializer_list<const char*> aNames)
            {
                for (const auto* name : aNames)
                {
                    aFunction = reinterpret_cast<std::remove_reference_t<decltype(aFunction)>>(GetProcAddress(module, name));
                    if (aFunction)
                        return;
                }
                missing += (missing.empty() ? "" : ", ") + std::string(*aNames.begin());
            };
            resolve(s_api.GetHSteamUser, {"SteamAPI_GetHSteamUser"});
            resolve(s_api.FindOrCreateUserInterface, {"SteamInternal_FindOrCreateUserInterface"});
            resolve(s_api.GetMatchmaking, {"SteamAPI_SteamMatchmaking_v009"});
            resolve(s_api.CreateLobby, {"SteamAPI_ISteamMatchmaking_CreateLobby"});
            resolve(s_api.SetLobbyData, {"SteamAPI_ISteamMatchmaking_SetLobbyData"});
            resolve(s_api.LeaveLobby, {"SteamAPI_ISteamMatchmaking_LeaveLobby"});
            resolve(s_api.AddRequestLobbyListStringFilter, {"SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter"});
            resolve(s_api.AddRequestLobbyListDistanceFilter, {"SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter"});
            resolve(s_api.AddRequestLobbyListResultCountFilter,
                    {"SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter"});
            resolve(s_api.RequestLobbyList, {"SteamAPI_ISteamMatchmaking_RequestLobbyList"});
            resolve(s_api.GetLobbyByIndex, {"SteamAPI_ISteamMatchmaking_GetLobbyByIndex"});
            resolve(s_api.GetLobbyData, {"SteamAPI_ISteamMatchmaking_GetLobbyData"});
            resolve(s_api.GetUtils, {"SteamAPI_SteamUtils_v010", "SteamAPI_SteamUtils_v009"});
            resolve(s_api.IsAPICallCompleted, {"SteamAPI_ISteamUtils_IsAPICallCompleted"});
            resolve(s_api.GetAPICallResult, {"SteamAPI_ISteamUtils_GetAPICallResult"});

            if (!missing.empty())
                s_error = "the game's Steam API lacks " + missing;
        }

        if (!s_error.empty())
            spdlog::warn("[Steam] Not used: {}", s_error);
    }

    aWhy = s_error;
    return s_error.empty() ? &s_api : nullptr;
}

const Api* GetApi()
{
    std::string why;
    return LoadApi(why);
}

ISteamNetworkingSockets* s_pSockets = nullptr;
ISteamNetworkingUtils* s_pNetworkingUtils = nullptr;
Matchmaking* s_pMatchmaking = nullptr;
Utils* s_pUtils = nullptr;
} // namespace

bool SteamLobby::Init(std::string& aWhy)
{
    if (s_pSockets)
        return true;

    const auto* pApi = LoadApi(aWhy);
    if (!pApi)
        return false;

    const auto user = pApi->GetHSteamUser();
    if (user == 0)
    {
        aWhy = "Steam isn't running";
        return false;
    }

    // The interfaces this mod's networking library declares (GameNetworkingSockets: "SteamNetworkingSockets012",
    // "SteamNetworkingUtils004"), from the Steam client itself, so they don't depend on the game's steam_api64.dll.
    auto* pSockets = static_cast<ISteamNetworkingSockets*>(
        pApi->FindOrCreateUserInterface(user, STEAMNETWORKINGSOCKETS_INTERFACE_VERSION));
    auto* pNetworkingUtils = static_cast<ISteamNetworkingUtils*>(
        pApi->FindOrCreateUserInterface(0, STEAMNETWORKINGUTILS_INTERFACE_VERSION));
    auto* pMatchmaking = pApi->GetMatchmaking();
    auto* pUtils = pApi->GetUtils();
    if (!pSockets || !pNetworkingUtils || !pMatchmaking || !pUtils)
    {
        aWhy = "Steam has no networking or matchmaking interface for this game";
        return false;
    }

    // Connections by address (join_address, or the host's address) aren't authenticated by Steam: allow them.
    pNetworkingUtils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 2);
    // Reach the relays now, so the first connection doesn't wait for it.
    pNetworkingUtils->InitRelayNetworkAccess();

    s_pSockets = pSockets;
    s_pNetworkingUtils = pNetworkingUtils;
    s_pMatchmaking = pMatchmaking;
    s_pUtils = pUtils;

    spdlog::info("[Steam] Using Steam's networking (SteamID {})", GetSteamId());
    return true;
}

ISteamNetworkingSockets* SteamLobby::GetSockets()
{
    return s_pSockets;
}

ISteamNetworkingUtils* SteamLobby::GetNetworkingUtils()
{
    return s_pNetworkingUtils;
}

uint64_t SteamLobby::GetSteamId()
{
    SteamNetworkingIdentity identity{};
    if (s_pSockets && s_pSockets->GetIdentity(&identity))
        return identity.GetSteamID64();
    return 0;
}

std::string SteamLobby::KeyFor(const std::string& acPassword)
{
    // Not the password itself: lobby data is readable by any Steam user of the game. SHA-256 through the oldest
    // form of the Windows API (the newer one-call form depends on the targeted Windows version).
    std::string text = "cp2077-coop lobby:" + acPassword;
    uint8_t digest[32]{};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    const bool hashed =
        BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0)) &&
        BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(text.data()), static_cast<ULONG>(text.size()), 0)) &&
        BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash)
        BCryptDestroyHash(hash);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!hashed)
    {
        spdlog::error("[Steam] Couldn't hash the password");
        return {};
    }

    std::string key;
    for (size_t i = 0; i < 16; ++i)
        key += fmt::format("{:02x}", digest[i]);
    return key;
}

void SteamLobby::Create(const std::string& acKey, const std::string& acHostName, uint32_t aMaxPlayers)
{
    Leave();

    const auto* pApi = GetApi();
    if (!pApi || !s_pMatchmaking)
        return;

    m_key = acKey;
    m_hostName = acHostName;
    m_call = pApi->CreateLobby(s_pMatchmaking, kLobbyTypePublic, static_cast<int>(aMaxPlayers));
    m_state = State::kCreating;
    m_deadline = std::chrono::steady_clock::now() + kCreateTimeout;
}

void SteamLobby::Find(const std::string& acKey)
{
    Leave();

    const auto* pApi = GetApi();
    if (!pApi || !s_pMatchmaking)
        return;

    pApi->AddRequestLobbyListStringFilter(s_pMatchmaking, kKeyField, acKey.c_str(), kLobbyComparisonEqual);
    pApi->AddRequestLobbyListDistanceFilter(s_pMatchmaking, kLobbyDistanceWorldwide);
    pApi->AddRequestLobbyListResultCountFilter(s_pMatchmaking, 10);
    m_call = pApi->RequestLobbyList(s_pMatchmaking);
    m_state = State::kFinding;
    m_deadline = std::chrono::steady_clock::now() + kFindTimeout;
}

void SteamLobby::Leave()
{
    const auto* pApi = GetApi();
    if (pApi && s_pMatchmaking)
    {
        if (m_state == State::kInLobby && m_lobby != 0)
        {
            pApi->LeaveLobby(s_pMatchmaking, m_lobby);
            spdlog::info("[Steam] Left lobby {}", m_lobby);
        }
        else if (m_state == State::kCreating && m_call != 0)
        {
            // Still being created: leave it once it is.
            m_abandoned.push_back(m_call);
        }
    }

    m_state = State::kIdle;
    m_call = 0;
    m_lobby = 0;
}

std::optional<SteamLobby::Result> SteamLobby::Update()
{
    const auto* pApi = GetApi();
    if (!pApi || !s_pUtils)
        return std::nullopt;

    // Lobbies created after we stopped wanting them.
    std::erase_if(m_abandoned, [&](SteamAPICall aCall) {
        bool failed = false;
        if (!pApi->IsAPICallCompleted(s_pUtils, aCall, &failed))
            return false;
        LobbyCreated created{};
        if (pApi->GetAPICallResult(s_pUtils, aCall, &created, sizeof(created), LobbyCreated::kCallback, &failed) &&
            !failed && created.Result == kResultOK)
            pApi->LeaveLobby(s_pMatchmaking, created.SteamIDLobby);
        return true;
    });

    if (m_state != State::kCreating && m_state != State::kFinding)
        return std::nullopt;

    Result result;
    bool failed = false;
    if (m_call == 0)
    {
        m_state = State::kIdle;
        result.Error = "Steam didn't take the request";
        return result;
    }
    if (!pApi->IsAPICallCompleted(s_pUtils, m_call, &failed))
    {
        if (std::chrono::steady_clock::now() < m_deadline)
            return std::nullopt;
        if (m_state == State::kCreating)
            m_abandoned.push_back(m_call);
        m_state = State::kIdle;
        m_call = 0;
        result.Error = "Steam didn't answer";
        return result;
    }

    if (m_state == State::kCreating)
    {
        LobbyCreated created{};
        if (!pApi->GetAPICallResult(s_pUtils, m_call, &created, sizeof(created), LobbyCreated::kCallback, &failed) ||
            failed || created.Result != kResultOK)
        {
            m_state = State::kIdle;
            m_call = 0;
            result.Error = fmt::format("Steam couldn't open a lobby (result {})", created.Result);
            return result;
        }

        m_lobby = created.SteamIDLobby;
        m_state = State::kInLobby;
        m_call = 0;
        pApi->SetLobbyData(s_pMatchmaking, m_lobby, kKeyField, m_key.c_str());
        pApi->SetLobbyData(s_pMatchmaking, m_lobby, kHostIdField, std::to_string(GetSteamId()).c_str());
        pApi->SetLobbyData(s_pMatchmaking, m_lobby, kHostNameField, m_hostName.c_str());
        spdlog::info("[Steam] Opened lobby {}", m_lobby);

        result.Ok = true;
        result.HostSteamId = GetSteamId();
        result.HostName = m_hostName;
        return result;
    }

    // Finding
    m_state = State::kIdle;
    LobbyMatchList list{};
    const auto call = m_call;
    m_call = 0;
    if (!pApi->GetAPICallResult(s_pUtils, call, &list, sizeof(list), LobbyMatchList::kCallback, &failed) || failed)
    {
        result.Error = "Steam couldn't search the lobbies";
        return result;
    }

    const auto mine = GetSteamId();
    for (uint32_t i = 0; i < list.LobbiesMatching; ++i)
    {
        const auto lobby = pApi->GetLobbyByIndex(s_pMatchmaking, static_cast<int>(i));
        const char* host = pApi->GetLobbyData(s_pMatchmaking, lobby, kHostIdField);
        const auto hostId = host ? std::strtoull(host, nullptr, 10) : 0;
        if (hostId == 0 || hostId == mine)
            continue;

        ++result.Matches;
        if (!result.Ok)
        {
            const char* name = pApi->GetLobbyData(s_pMatchmaking, lobby, kHostNameField);
            result.Ok = true;
            result.HostSteamId = hostId;
            result.HostName = name && *name ? name : "the host";
        }
    }
    if (!result.Ok)
        result.Error = "no co-op session with this password on Steam";
    return result;
}
