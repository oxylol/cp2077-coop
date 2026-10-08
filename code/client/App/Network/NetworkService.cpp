#include "NetworkService.h"

#include "App/World/NetworkWorldSystem.h"
#include "Game/CustomizationState.h"
#include "Game/Utils.h"
#include "RED4ext/Scripting/Natives/Generated/Vector4.hpp"
#include "RED4ext/Scripting/Natives/Generated/game/Object.hpp"
#include "App/World/AppearanceSystem.h"
#include "App/Settings.h"
#include "Game/CharacterCustomizationSystem.h"

NetworkService::NetworkService()
    : Client(client::kIdentifier, server::kIdentifier)
    , m_lastUpdate(std::chrono::steady_clock::now())
{
    BindMessageHandlers();
}

NetworkService::~NetworkService()
{
}

void NetworkService::BindMessageHandlers()
{
    GetSink<server::AuthenticationResponse>().connect<&NetworkService::HandleAuthentication>(this);
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
    request.set_token("test");
    request.set_username(Settings::Get().name.c_str());
    request.set_client_protocol(client::kIdentifier);
    request.set_server_protocol(server::kIdentifier);

    Send(request);
}

void NetworkService::OnDisconnected(EDisconnectReason aReason)
{
    spdlog::info("Disconnected from server {}", static_cast<uint32_t>(aReason));
    Red::GetGameSystem<NetworkWorldSystem>()->OnDisconnected(aReason);

    m_authenticated = false;
}

void NetworkService::OnUpdate()
{
    m_dispatcher.update();

    Red::GetGameSystem<NetworkWorldSystem>()->Update(GetClock().GetCurrentTick());
}

void NetworkService::OnGameUpdate(RED4ext::CGameApplication* apApp)
{
    Update();
}

void NetworkService::HandleAuthentication(const PacketEvent<server::AuthenticationResponse>& aResponse)
{
    if (!aResponse.get_success())
    {
        spdlog::error("Authentication failed: {}", aResponse.get_error());
        Close();
        return;
    }

    m_settings = aResponse.get_settings();

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
