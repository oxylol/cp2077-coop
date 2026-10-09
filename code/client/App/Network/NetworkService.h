#pragma once

#include "Core/Buffer.h"
#include "Core/ScratchAllocator.h"
#include "Core/Foundation/Feature.hpp"
#include "Network/Client.h"
#include "Network/Packet.h"
#include "App/World/WorldSync.h"
#include "SteamLobby.h"

template <typename T> struct DebugType;

template <typename T>
concept NetworkMessage = requires(T a, Buffer::Writer writer, Buffer::Reader reader)
{
    {
        a.serialize(writer)
    } -> std::convertible_to<bool>;
    {
        a.deserialize(reader)
    } -> std::convertible_to<bool>;
};

struct NetworkService final
    : Client
    , Core::Feature
{
    NetworkService();
    ~NetworkService() override;

    template <NetworkMessage T>
    bool Send(const T& acMessage);

    template <NetworkMessage T>
    auto GetSink() noexcept { return m_dispatcher.sink<PacketEvent<T>>(); }

    template <auto Func, typename... T>
    auto RegisterHandler(T&&... args) noexcept
    {
        using MessageType = typename std::remove_cv_t<std::remove_reference_t < typename std::tuple_element<0, typename details::signature<decltype(Func)>::type>::type >>::Type;

        static_assert(NetworkMessage<MessageType>, "Handler should take a NetworkMessage as first parameter!");

        return m_dispatcher.sink<PacketEvent<MessageType>>().connect<Func>(std::forward<T>(args)...);
    }

    entt::dispatcher& GetDispatcher() { return m_dispatcher; }
    const server::Settings& GetServerSettings() const { return m_settings; }

    // The co-op session (in the game: hold "/" to host, "." to join, "/" again to leave; redscript
    // MultiplayerGameController). Host runs the session in this game (HostSession.h) and joins it. On Steam, guests
    // find it by the password (SteamLobby) and connect through Steam; otherwise by address (join_address).
    void Host();
    void Join();
    void Leave();
    bool IsHosting() const noexcept { return !m_hostToken.empty(); }

    TP_NOCOPYMOVE(NetworkService);

protected:
    void BindMessageHandlers();

    void OnConsume(const void* apData, uint32_t aSize) override;
    void OnConnected() override;
    void OnDisconnected(EDisconnectReason aReason) override;
    void OnUpdate() override;
    void OnGameUpdate(RED4ext::CGameApplication* apApp) override;

    void HandleAuthentication(const PacketEvent<server::AuthenticationResponse>& aResponse);
    void HandlePlayerJoined(const PacketEvent<server::NotifyPlayerJoined>& aMessage);
    void HandlePlayerLeft(const PacketEvent<server::NotifyPlayerLeft>& aMessage);
    void HandleWorldState(const PacketEvent<server::NotifyWorldState>& aMessage);

    // Steam's networking for the session when it can be used (coop.ini steam), the mod's own otherwise (aWhy).
    static bool UseSteam(std::string& aWhy);
    void OnLobbyResult(const SteamLobby::Result& acResult);

    // A line in the middle of the screen, also logged.
    static void ShowMessage(const std::string& acText);

    static ScratchAllocator& GetScratch();

private:

    entt::dispatcher m_dispatcher;
    server::Settings m_settings;

    // Connecting or connected.
    bool m_busy = false;
    // Accepted by the session.
    bool m_authenticated = false;
    // Leave() is closing the connection.
    bool m_leaving = false;
    // The session refused us (the reason is already on screen).
    bool m_refused = false;
    // Set while hosting: our own game proves it's the host with it.
    std::string m_hostToken;
    // Joining by address: where to. Joining through Steam: whose session.
    std::string m_address;
    std::string m_hostName;
    // This session goes through Steam.
    bool m_viaSteam = false;
    SteamLobby m_lobby;
    WorldSync m_worldSync;
};

template <NetworkMessage T>
bool NetworkService::Send(const T& acMessage)
{
    ScopedResetAllocator _{GetScratch()};

    Buffer buffer(1 << 18);
    Buffer::Writer writer(&buffer);
    writer.WriteBits(0, 8); // Skip the first byte as it is used by packet

    client::Serializer::Process(writer, acMessage);

    PacketView packet(reinterpret_cast<char*>(buffer.GetWriteData()), (uint32_t)writer.Size());
    Client::Send(&packet, T::kReliable ? kReliable : kUnreliable);

    return true;
}
