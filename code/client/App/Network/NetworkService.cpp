#include "NetworkService.h"

#include "App/World/NetworkWorldSystem.h"
#include "Game/CustomizationState.h"
#include "Game/Utils.h"
#include "RED4ext/Scripting/Natives/Generated/Vector4.hpp"
#include "RED4ext/Scripting/Natives/Generated/game/Object.hpp"
#include "App/World/AppearanceSystem.h"
#include "App/Settings.h"
#include "Game/CharacterCustomizationSystem.h"

#include <HostSession.h>

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

void NetworkService::Host()
{
    if (m_busy)
        return;

    const auto& settings = Settings::Get();

    HostSession::Settings session;
    session.Port = settings.port;
    session.MaxPlayers = settings.maxPlayers;
    session.Password = settings.password.c_str();
    session.HostToken = MakeToken();

    if (!HostSession::Start(session))
    {
        ShowMessage(fmt::format("Couldn't host: no free port from {} on.", settings.port));
        return;
    }

    // Our own game joins the session like everyone else, through the loopback address.
    m_hostToken = session.HostToken;
    m_address = fmt::format("127.0.0.1:{}", HostSession::GetPort());
    m_busy = true;
    m_refused = false;
    ShowMessage("Starting a co-op session...");
    Connect(m_address);
}

void NetworkService::Join()
{
    if (m_busy)
        return;

    m_hostToken.clear();
    m_address = Settings::Get().joinAddress.c_str();
    m_busy = true;
    m_refused = false;
    ShowMessage(fmt::format("Joining the co-op session at {}...", m_address));
    if (!Connect(m_address))
    {
        m_busy = false;
        ShowMessage(fmt::format("Couldn't join: \"{}\" isn't an address (join_address in {}).", m_address,
                                Settings::Get().iniPath.string()));
    }
}

void NetworkService::Leave()
{
    m_leaving = true;
    Close();
    m_leaving = false;

    // Still resolving the address: there was no connection yet to report its end.
    m_busy = false;
    m_authenticated = false;

    if (HostSession::IsRunning())
        HostSession::Stop();
    m_hostToken.clear();
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
    // The session this game hosts, if any, then our connection to it (or to the host).
    HostSession::Update();
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

    if (IsHosting())
        ShowMessage(fmt::format("Hosting a co-op session (port {}). Guests hold \".\" to join.", HostSession::GetPort()));
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
