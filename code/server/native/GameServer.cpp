#include "GameServer.h"

#include "Game/Level.h"
#include "PlayerManager.h"

GameServer* GServer = nullptr;

GameServer::GameServer(const Config& acConfig)
    : Server(client::kIdentifier, server::kIdentifier)
    , m_config(acConfig)
    , m_lastUpdate(std::chrono::steady_clock::now())
{
    GServer = this;

    // Runs inside the game's frame: never sleep there.
    SetBlocking(false);

    // A port taken by something else (another game on this PC, for instance) moves the session to the next one.
    uint16_t port = m_config.Port;
    for (int attempt = 0; attempt < 10; ++attempt, ++port)
    {
        if (IsUdpPortFree(port) && Host(port, m_config.TickRate))
            break;
        Close();
        spdlog::warn("[Session] Port {} is already in use, trying {}", port, port + 1);
    }

    if (!IsListening())
    {
        spdlog::error("[Session] Couldn't open a port between {} and {}", m_config.Port, port - 1);
        return;
    }

    if (m_config.SteamP2P && !HostP2P())
        spdlog::error("[Session] Couldn't open the Steam (P2P) listen socket; only addresses can join");

    m_pWorld = MakeUnique<World>();

    RegisterHandler<&GameServer::HandleAuthentication>(this);
    RegisterHandler<&GameServer::HandleReportWorldState>(this);

    spdlog::info("[Session] Hosting on port {}{} (up to {} players, {})", GetPort(),
                 IsListeningP2P() ? " and through Steam" : "", m_config.MaxPlayer,
                 m_config.Password.empty() ? "no password" : "password set");
}

GameServer::~GameServer()
{
    Close();
    if (GServer == this)
        GServer = nullptr;
}

void GameServer::OnUpdate()
{
    const auto now = std::chrono::steady_clock::now();
    const auto delta = now - m_lastUpdate;
    m_lastUpdate = now;

    m_tasks.Drain();
    m_dispatcher.update();

    if (m_pWorld)
        m_pWorld->Update(std::chrono::duration_cast<std::chrono::duration<float>>(delta).count());
}

void GameServer::OnConsume(const void* apData, uint32_t aSize, ConnectionId aConnectionId)
{
    // ReSharper disable once CppCStyleCast
    ViewBuffer buffer((uint8_t*)apData, aSize);  // NOLINT(clang-diagnostic-cast-qual)
    Buffer::Reader reader(&buffer);

    if (const auto result = client::Deserializer::Process(reader, aConnectionId, m_dispatcher); !result)
    {
        spdlog::error("[Session] Failed to deserialize packet from {:x}", aConnectionId);
    }
}

void GameServer::OnConnection(ConnectionId aHandle)
{
    char address[SteamNetworkingIPAddr::k_cchMaxString] = {};
    const auto info = GetConnectionInfo(aHandle);
    info.m_addrRemote.ToString(address, sizeof(address), true);

    spdlog::info("[Session] Connection from {} (id {:x})", address, aHandle);
}

void GameServer::OnDisconnection(ConnectionId aConnectionId, EDisconnectReason aReason)
{
    spdlog::info("[Session] Connection {:x} ended (reason {})", aConnectionId, static_cast<uint32_t>(aReason));

    if (!m_pWorld)
        return;

    auto* pPlayerManager = GetWorld()->get_mut<PlayerManager>();

    if (const auto player = pPlayerManager->GetByConnectionId(aConnectionId))
    {
        std::string username;
        pPlayerManager->Remove(player);

        if (auto* pPlayerComponent = player.get<PlayerComponent>())
        {
            username = pPlayerComponent->Username;
            GetWorld()->get_mut<Level>()->Remove(pPlayerComponent->Puppet);
        }

        player.destruct();

        server::NotifyPlayerLeft left;
        left.set_username(username.c_str());
        SendToPlayers(left);

        spdlog::info("[Session] {} left, {}/{} player(s)", username, pPlayerManager->Count(), m_config.MaxPlayer);
    }
}

void GameServer::Refuse(ConnectionId aConnectionId, const char* acReason)
{
    server::AuthenticationResponse response;
    response.set_success(false);
    response.set_error(acReason);
    Send(aConnectionId, response);
    Kick(aConnectionId);
}

void GameServer::HandleAuthentication(const PacketEvent<client::AuthenticationRequest>& aRequest)
{
    const auto connection = aRequest.ConnectionId;

    if (aRequest.get_client_protocol() != client::kIdentifier || aRequest.get_server_protocol() != server::kIdentifier)
    {
        spdlog::warn("[Session] {} has another version of the mod (protocol {:x}/{:x}, expected {:x}/{:x})",
                     aRequest.get_username(), aRequest.get_client_protocol(), aRequest.get_server_protocol(),
                     client::kIdentifier, server::kIdentifier);
        Refuse(connection, "The host has another version of the mod.");
        return;
    }

    const bool isHost = !m_config.HostToken.empty() && aRequest.get_host_token() == m_config.HostToken.c_str();

    if (!isHost && aRequest.get_password() != m_config.Password.c_str())
    {
        spdlog::warn("[Session] {} sent a wrong password", aRequest.get_username());
        Refuse(connection, "Wrong password: it has to match the host's (coop.ini).");
        return;
    }

    auto* pPlayerManager = GetWorld()->get_mut<PlayerManager>();
    if (!isHost && pPlayerManager->Count() >= m_config.MaxPlayer)
    {
        Refuse(connection, "The session is full.");
        return;
    }

    const auto host = pPlayerManager->GetHost();
    const auto* pHost = host ? host.get<PlayerComponent>() : nullptr;

    server::AuthenticationResponse response;
    response.set_success(true);

    server::Settings settings;
    settings.set_update_rate(m_config.UpdateRate);
    response.set_settings(settings);
    response.set_host_name(isHost ? aRequest.get_username() : (pHost ? pHost->Username.c_str() : ""));

    Send(connection, response);

    // The others learn about the newcomer before it's added, so it doesn't get its own notice.
    server::NotifyPlayerJoined joined;
    joined.set_username(aRequest.get_username());
    SendToPlayers(joined);

    pPlayerManager->Create(connection, aRequest.get_username(), isHost);

    if (!isHost && m_worldState)
        Send(connection, *m_worldState);

    spdlog::info("[Session] {} joined{}, {}/{} player(s)", aRequest.get_username(), isHost ? " (host)" : "",
                 pPlayerManager->Count(), m_config.MaxPlayer);
}

void GameServer::HandleReportWorldState(const PacketEvent<client::ReportWorldState>& aReport)
{
    const auto player = GetWorld()->get_mut<PlayerManager>()->GetByConnectionId(aReport.ConnectionId);
    const auto* pPlayer = player ? player.get<PlayerComponent>() : nullptr;

    // Only the host's world counts.
    if (!pPlayer || !pPlayer->IsHost)
        return;

    server::NotifyWorldState state;
    state.set_game_time(aReport.get_game_time());
    state.set_weather(aReport.get_weather());
    m_worldState = state;

    SendToPlayers(state, aReport.ConnectionId);
}

ScratchAllocator& GameServer::GetScratch()
{
    thread_local ScratchAllocator s_allocator{1 << 19};
    return s_allocator;
}
