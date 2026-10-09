#include "NetworkService.h"

#include "App/World/NetworkWorldSystem.h"
#include "Game/CustomizationState.h"
#include "Game/Utils.h"
#include "RED4ext/Scripting/Natives/Generated/Vector4.hpp"
#include "RED4ext/Scripting/Natives/Generated/game/Object.hpp"
#include "App/World/AppearanceSystem.h"
#include "App/Settings.h"
#include "Game/CharacterCustomizationSystem.h"
#include "Support/Spdlog/CrashLog.h"

#include <HostSession.h>
#include <Network/SteamInterface.h>

#include <random>

namespace
{
// The host token: only the host's own game knows it.
std::string MakeToken()
{
    std::random_device device;
    std::mt19937_64 random(static_cast<uint64_t>(device()) << 32 | device());
    return fmt::format("{:016x}{:016x}", random(), random());
}
} // namespace

NetworkService::NetworkService()
    : Client(client::kIdentifier, server::kIdentifier)
{
    BindMessageHandlers();
}

NetworkService::~NetworkService()
{
    // The game is closing: a session it hosts ends now (the players' connections time out).
    m_lobby.Leave();
    HostSession::Shutdown();
}

void NetworkService::BindMessageHandlers()
{
    GetSink<server::AuthenticationResponse>().connect<&NetworkService::HandleAuthentication>(this);
    GetSink<server::NotifyPlayerJoined>().connect<&NetworkService::HandlePlayerJoined>(this);
    GetSink<server::NotifyPlayerLeft>().connect<&NetworkService::HandlePlayerLeft>(this);
    GetSink<server::NotifyWorldState>().connect<&NetworkService::HandleWorldState>(this);
}

void NetworkService::ShowMessage(const std::string& acText)
{
    spdlog::info("[Co-op] {}", acText);
    if (const auto pWorld = Red::GetGameSystem<NetworkWorldSystem>())
        pWorld->ShowMessage(acText);
}

bool NetworkService::UseSteam(std::string& aWhy)
{
    const bool steam = Settings::Get().steam && SteamLobby::Init(aWhy);
    if (!Settings::Get().steam)
        aWhy = "Steam is off in coop.ini";
    else if (!steam)
        spdlog::info("[Co-op] Not through Steam: {}", aWhy);

    // The session and the connections to it run on Steam's networking or on the mod's own.
    SteamInterface::SetSockets(steam ? SteamLobby::GetSockets() : nullptr,
                               steam ? SteamLobby::GetNetworkingUtils() : nullptr);
    return steam;
}

void NetworkService::Host()
{
    if (m_busy)
        return;

    const auto& settings = Settings::Get();
    const std::string password = settings.password.c_str();

    std::string why;
    const bool steam = UseSteam(why);
    if (steam && (password.empty() || password == Settings::kDefaultPassword))
    {
        ShowMessage(fmt::format("Set a password of your own in {} first: on Steam, anyone with the same password "
                                "finds your session.", settings.iniPath.string()));
        return;
    }

    HostSession::Settings session;
    session.Port = settings.port;
    session.MaxPlayers = settings.maxPlayers;
    session.Password = password;
    session.HostToken = MakeToken();
    session.SteamP2P = steam;

    if (!HostSession::Start(session))
    {
        ShowMessage(fmt::format("Couldn't host: no free port from {} on.", settings.port));
        return;
    }

    // Our own game joins the session like everyone else, through a connection within this process.
    ISteamNetworkingSockets* pSockets = nullptr;
    HSteamNetConnection connection = k_HSteamNetConnection_Invalid;
    if (!HostSession::OpenLocalConnection(pSockets, connection))
    {
        HostSession::Stop();
        ShowMessage("Couldn't host: the session didn't let this game in.");
        return;
    }

    m_hostToken = session.HostToken;
    m_address.clear();
    m_hostName.clear();
    m_viaSteam = steam;
    m_notThroughSteam = Settings::Get().steam && !steam ? why : std::string{};
    m_busy = true;
    m_refused = false;

    if (steam && HostSession::IsReachableThroughSteam())
        m_lobby.Create(SteamLobby::KeyFor(password), settings.name.c_str(), settings.maxPlayers);
    else if (steam)
        ShowMessage("Steam didn't open the session to other players: they need your address (join_address).");

    Adopt(pSockets, connection);
}

void NetworkService::Join()
{
    if (m_busy)
        return;

    const auto& settings = Settings::Get();
    const std::string password = settings.password.c_str();

    std::string why;
    const bool steam = UseSteam(why);

    m_hostToken.clear();
    m_hostName.clear();
    m_refused = false;

    // By address
    if (!settings.joinAddress.empty())
    {
        m_address = settings.joinAddress.c_str();
        m_viaSteam = false;
        m_busy = true;
        ShowMessage(fmt::format("Joining the co-op session at {}...", m_address));
        if (!Connect(m_address))
        {
            m_busy = false;
            ShowMessage(fmt::format("Couldn't join: \"{}\" isn't an address (join_address in {}).", m_address,
                                    settings.iniPath.string()));
        }
        return;
    }

    // Through Steam, by the password
    if (!steam)
    {
        ShowMessage(fmt::format("Couldn't join: {}. Set the host's address as join_address in {}.", why,
                                settings.iniPath.string()));
        return;
    }
    if (password.empty() || password == Settings::kDefaultPassword)
    {
        ShowMessage(fmt::format("Set the host's password in {} first.", settings.iniPath.string()));
        return;
    }

    m_address.clear();
    m_viaSteam = true;
    m_busy = true;
    ShowMessage("Looking for the co-op session with your password on Steam...");
    m_lobby.Find(SteamLobby::KeyFor(password));
}

void NetworkService::Leave()
{
    m_leaving = true;
    Close();
    m_leaving = false;

    // Still finding the host or resolving the address: there was no connection yet to report its end.
    m_busy = false;
    m_authenticated = false;

    m_lobby.Leave();
    if (HostSession::IsRunning())
        HostSession::Stop();
    m_hostToken.clear();
}

void NetworkService::OnLobbyResult(const SteamLobby::Result& acResult)
{
    if (IsHosting())
    {
        // The lobby guests find us by
        if (!acResult.Ok)
            ShowMessage(fmt::format("{}: guests can only join with your address (join_address).", acResult.Error));
        return;
    }

    // Looking for the host's lobby
    if (!m_busy)
        return; // left meanwhile

    if (!acResult.Ok)
    {
        m_busy = false;
        ShowMessage(fmt::format("Couldn't join: {}. Is the host's session running, with the same password?",
                                acResult.Error));
        return;
    }

    if (acResult.Matches > 1)
        spdlog::warn("[Co-op] {} sessions on Steam have this password; joining {}'s", acResult.Matches,
                     acResult.HostName);

    m_hostName = acResult.HostName;
    ShowMessage(fmt::format("Joining {}'s co-op session through Steam...", m_hostName));
    if (!ConnectP2P(acResult.HostSteamId))
    {
        m_busy = false;
        ShowMessage("Couldn't join: Steam didn't open the connection.");
    }
}

void NetworkService::OnConsume(const void* apData, uint32_t aSize)
{
    ViewBuffer buf((uint8_t*)apData, aSize);
    Buffer::Reader reader(&buf);

    if(!server::Deserializer::Process(reader, 0, m_dispatcher))
    {
        spdlog::error("Failed to deserialize a message from the server.");
    }
}

void NetworkService::OnConnected()
{
    spdlog::info("Connected to server.");

    client::AuthenticationRequest request;
    request.set_host_token(m_hostToken.c_str());
    request.set_password(Settings::Get().password.c_str());
    request.set_username(Settings::Get().name.c_str());
    request.set_client_protocol(client::kIdentifier);
    request.set_server_protocol(server::kIdentifier);

    Send(request);
}

void NetworkService::OnDisconnected(EDisconnectReason aReason)
{
    spdlog::info("Disconnected from the session ({})", static_cast<uint32_t>(aReason));
    Red::GetGameSystem<NetworkWorldSystem>()->OnDisconnected(aReason);

    const bool wasInSession = m_authenticated;
    const bool wasHosting = IsHosting();
    m_authenticated = false;
    m_busy = false;
    m_worldSync.Reset();

    if (m_refused)
    {
        // The reason is on screen already.
    }
    else if (m_leaving)
    {
        ShowMessage(wasHosting ? "You ended the co-op session." : "You left the co-op session.");
    }
    else if (!wasInSession)
    {
        if (m_viaSteam && !m_hostName.empty())
            ShowMessage(fmt::format("Couldn't reach {}'s session through Steam.", m_hostName));
        else
            ShowMessage(fmt::format("Couldn't reach the host at {}.", m_address));
    }
    else if (aReason == kKicked)
    {
        ShowMessage("The co-op session ended.");
    }
    else
    {
        ShowMessage("Lost the connection to the co-op session.");
    }

    // Our own connection to the session we host is gone: end it for everyone.
    if (wasHosting && !m_leaving)
    {
        m_lobby.Leave();
        HostSession::Stop();
        m_hostToken.clear();
    }
}

void NetworkService::OnUpdate()
{
    m_dispatcher.update();

    Red::GetGameSystem<NetworkWorldSystem>()->Update(GetClock().GetCurrentTick());
}

void NetworkService::OnGameUpdate(RED4ext::CGameApplication* apApp)
{
    Support::CrashLog::Heartbeat();

    // The session this game hosts, if any, then our connection to it (or to the host).
    HostSession::Update();
    if (auto result = m_lobby.Update())
        OnLobbyResult(*result);
    Update();

    if (m_authenticated && IsHosting())
        m_worldSync.Report(*this);
}

void NetworkService::HandlePlayerJoined(const PacketEvent<server::NotifyPlayerJoined>& aMessage)
{
    ShowMessage(fmt::format("{} joined the co-op session.", aMessage.get_username()));
}

void NetworkService::HandlePlayerLeft(const PacketEvent<server::NotifyPlayerLeft>& aMessage)
{
    ShowMessage(fmt::format("{} left the co-op session.", aMessage.get_username()));
}

void NetworkService::HandleWorldState(const PacketEvent<server::NotifyWorldState>& aMessage)
{
    if (!IsHosting())
        m_worldSync.Apply(aMessage.get_game_time(), aMessage.get_weather());
}

void NetworkService::HandleAuthentication(const PacketEvent<server::AuthenticationResponse>& aResponse)
{
    if (!aResponse.get_success())
    {
        spdlog::error("Authentication failed: {}", aResponse.get_error());
        m_refused = true;
        ShowMessage(fmt::format("Couldn't join: {}", aResponse.get_error()));
        Close();
        return;
    }

    m_settings = aResponse.get_settings();
    m_authenticated = true;

    if (IsHosting() && m_viaSteam)
        ShowMessage("Hosting a co-op session. Guests with your password hold \".\" to join.");
    else if (IsHosting() && !m_notThroughSteam.empty())
        ShowMessage(fmt::format("Hosting a co-op session on port {}, not through Steam ({}). Guests hold \".\" to join "
                                "with your address.", HostSession::GetPort(), m_notThroughSteam));
    else if (IsHosting())
        ShowMessage(fmt::format("Hosting a co-op session on port {}. Guests hold \".\" to join with your address.",
                                HostSession::GetPort()));
    else
        ShowMessage(fmt::format("Joined {}'s co-op session.", aResponse.get_host_name()));

    Red::GetGameSystem<NetworkWorldSystem>()->OnConnected();

    client::SpawnCharacterRequest request;
    request.set_is_player(true);

    const auto system = Red::GetGameSystem<Game::PlayerSystem>();
    Red::Handle<Red::GameObject> player;
    system->GetLocalPlayerControlledGameObject(player);

    // Use worldTransform: localTransform is relative and stays near-origin, which made
    // the server spawn our puppet at ~(0, 3.6, 0) instead of our actual world position.
    const auto& cEntityPosition = player->placedComponent->worldTransform.Position;
    const auto cEntityRotation = Game::ToGlm(player->placedComponent->worldTransform.Orientation);

    common::Vector3 pos;
    pos.set_x(cEntityPosition.x);
    pos.set_y(cEntityPosition.y);
    pos.set_z(cEntityPosition.z);

    request.set_position(pos);
    request.set_rotation(cEntityRotation.z);
    request.set_cookie(0);

    auto appSystem = Red::GetGameSystem<NetworkWorldSystem>()->GetAppearanceSystem();
    request.set_equipment(appSystem->GetPlayerItems(player));


    // The handle at +0x78 is null during normal play on 2.31 (PR 58); FindLocalCustomizationState also looks for
    // the state elsewhere and logs where it found it (Game/CustomizationState.h).
    const auto state = FindLocalCustomizationState();

    if (state)
    {
        auto writer = CMPWriter();
        CharacterCustomizationState_Serialize(state.instance, &writer);
        spdlog::info("Got bytes: {}", writer.bytes.size());
        request.set_ccstate(writer.bytes);
    }
    else
    {
        spdlog::info("CustomizationState was null");
    }

    Send(request);
}

ScratchAllocator& NetworkService::GetScratch()
{
    thread_local ScratchAllocator s_allocator{1 << 19};
    return s_allocator;
}
