#pragma once

#include "Config.h"
#include "Game/World.h"
#include "Components/PlayerComponent.h"

template <typename T>
concept ServerMessage = requires(T a, Buffer::Writer writer, Buffer::Reader reader) {
    {
        a.serialize(writer)
    } -> std::convertible_to<bool>;
    {
        a.deserialize(reader)
    } -> std::convertible_to<bool>;
};

// The co-op session, run inside the host's game (HostSession.h). Update() is called once per frame on the game's
// main thread and never sleeps.
struct GameServer final : Server
{
    TP_NOCOPYMOVE(GameServer);

    explicit GameServer(const Config& acConfig);
    ~GameServer() override;

    // False when no port could be opened.
    bool IsRunning() const noexcept { return IsListening(); }

    template <ServerMessage T>
    bool Send(ConnectionId aConnectionId, const T& acMessage) const;
    // To every authenticated player except aExcept (0: nobody excepted).
    template <ServerMessage T>
    void SendToPlayers(const T& acMessage, ConnectionId aExcept = 0);

    template<ServerMessage T>
    auto GetSink() noexcept { return m_dispatcher.sink<PacketEvent<T>>(); }

    template <auto Func, typename... T> auto RegisterHandler(T&&... args) noexcept
    {
        using MessageType = typename std::remove_cv_t<std::remove_reference_t<typename std::tuple_element<0, typename details::signature<decltype(Func)>::type>::type>>::Type;

        static_assert(ServerMessage<MessageType>, "Handler should take a network message as first parameter!");

        return m_dispatcher.sink<PacketEvent<MessageType>>().template connect<Func>(std::forward<T>(args)...);
    }

    gsl::not_null<const Config*> GetConfig() const noexcept { return &m_config; }
    gsl::not_null<World*> GetWorld() noexcept { return m_pWorld.get(); }
    gsl::not_null<TaskQueue*> GetTaskQueue() noexcept { return &m_tasks; }

protected:
    void OnUpdate() override;
    void OnConsume(const void* apData, uint32_t aSize, ConnectionId aConnectionId) override;
    void OnConnection(ConnectionId aHandle) override;
    void OnDisconnection(ConnectionId aConnectionId, EDisconnectReason aReason) override;

    void HandleAuthentication(const PacketEvent<client::AuthenticationRequest>& aRequest);
    void HandleReportWorldState(const PacketEvent<client::ReportWorldState>& aReport);

    static ScratchAllocator& GetScratch();

private:
    void Refuse(ConnectionId aConnectionId, const char* acReason);

    Config m_config;
    UniquePtr<World> m_pWorld;
    TaskQueue m_tasks;
    std::chrono::steady_clock::time_point m_lastUpdate;
    entt::dispatcher m_dispatcher;

    // The host's last reported time of day and weather, for guests who join later.
    std::optional<server::NotifyWorldState> m_worldState;
};

template <ServerMessage T>
bool GameServer::Send(ConnectionId aConnectionId, const T& acMessage) const
{
    ScopedResetAllocator _{GetScratch()};

    Buffer buffer(1 << 18);
    Buffer::Writer writer(&buffer);
    writer.WriteBits(0, 8); // Skip the first byte as it is used by packet

    server::Serializer::Process(writer, acMessage);

    PacketView packet(reinterpret_cast<char*>(buffer.GetWriteData()), (uint32_t)writer.Size());
    Server::Send(aConnectionId, &packet, T::kReliable ? kReliable : kUnreliable);

    return true;
}

template <ServerMessage T>
void GameServer::SendToPlayers(const T& acMessage, ConnectionId aExcept)
{
    m_pWorld->each([&](const PlayerComponent& acPlayer) {
        if (acPlayer.Connection != aExcept)
            Send(acPlayer.Connection, acMessage);
    });
}

extern GameServer* GServer;
