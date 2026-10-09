#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include <Core/Buffer.h>
#include <Core/ScratchAllocator.h>
#include <Core/ViewBuffer.h>
#include <Network/Client.h>
#include <Network/Packet.h>
#include <Network/Server.h>
#include <ProtocolPCH.h>
#include <client.gen.h>
#include <server.gen.h>

#include <HostSession.h>

namespace
{
constexpr uint16_t kPort = 31778;

// What the game's NetworkService does, minus the game: authenticates, then records what the session sends.
struct TestClient final : Client
{
    TestClient(std::string aName, std::string aPassword, std::string aHostToken = {})
        : Client(client::kIdentifier, server::kIdentifier)
        , Name(std::move(aName))
        , Password(std::move(aPassword))
        , HostToken(std::move(aHostToken))
    {
        m_dispatcher.sink<PacketEvent<server::AuthenticationResponse>>().connect<&TestClient::OnAuthentication>(this);
        m_dispatcher.sink<PacketEvent<server::NotifyPlayerJoined>>().connect<&TestClient::OnJoined>(this);
        m_dispatcher.sink<PacketEvent<server::NotifyPlayerLeft>>().connect<&TestClient::OnLeft>(this);
        m_dispatcher.sink<PacketEvent<server::NotifyWorldState>>().connect<&TestClient::OnWorldState>(this);
    }

    template <class T> void SendMessage(const T& acMessage)
    {
        ScopedResetAllocator _{GetScratch()};
        Buffer buffer(1 << 16);
        Buffer::Writer writer(&buffer);
        writer.WriteBits(0, 8);
        client::Serializer::Process(writer, acMessage);
        PacketView packet(reinterpret_cast<char*>(buffer.GetWriteData()), static_cast<uint32_t>(writer.Size()));
        Client::Send(&packet, T::kReliable ? kReliable : kUnreliable);
    }

    void OnConnected() override
    {
        client::AuthenticationRequest request;
        request.set_username(Name.c_str());
        request.set_password(Password.c_str());
        request.set_host_token(HostToken.c_str());
        request.set_client_protocol(client::kIdentifier);
        request.set_server_protocol(server::kIdentifier);
        SendMessage(request);
    }

    void OnConsume(const void* apData, uint32_t aSize) override
    {
        ViewBuffer buffer(static_cast<uint8_t*>(const_cast<void*>(apData)), aSize);
        Buffer::Reader reader(&buffer);
        REQUIRE(server::Deserializer::Process(reader, 0, m_dispatcher));
    }

    void OnDisconnected(EDisconnectReason) override { Disconnected = true; }
    void OnUpdate() override { m_dispatcher.update(); }

    void OnAuthentication(const PacketEvent<server::AuthenticationResponse>& acResponse)
    {
        Accepted = acResponse.get_success();
        Error = acResponse.get_error().c_str();
        HostName = acResponse.get_host_name().c_str();
    }
    void OnJoined(const PacketEvent<server::NotifyPlayerJoined>& acEvent) { Joined.emplace_back(acEvent.get_username().c_str()); }
    void OnLeft(const PacketEvent<server::NotifyPlayerLeft>& acEvent) { Left.emplace_back(acEvent.get_username().c_str()); }
    void OnWorldState(const PacketEvent<server::NotifyWorldState>& acEvent)
    {
        WorldState = std::make_pair(acEvent.get_game_time(), acEvent.get_weather());
    }

    static ScratchAllocator& GetScratch()
    {
        thread_local ScratchAllocator s_allocator{1 << 17};
        return s_allocator;
    }

    std::string Name;
    std::string Password;
    std::string HostToken;
    std::optional<bool> Accepted;
    std::string Error;
    std::string HostName;
    std::vector<std::string> Joined;
    std::vector<std::string> Left;
    std::optional<std::pair<uint32_t, uint64_t>> WorldState;
    bool Disconnected = false;

private:
    entt::dispatcher m_dispatcher;
};

// Runs the session and the clients on this thread, as the game does once per frame, until aDone or 2 s.
bool Pump(const std::vector<TestClient*>& acClients, const std::function<bool()>& aDone)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < end)
    {
        HostSession::Update();
        for (auto* pClient : acClients)
            pClient->Update();
        if (aDone())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

std::string Address()
{
    return "127.0.0.1:" + std::to_string(HostSession::GetPort());
}
} // namespace

TEST_CASE("The host's game hosts a session, guests join it with the password")
{
    HostSession::Settings settings;
    settings.Port = kPort;
    settings.Password = "secret";
    settings.HostToken = "host-token";
    settings.MaxPlayers = 2;
    REQUIRE(HostSession::Start(settings));
    REQUIRE(HostSession::IsRunning());

    TestClient host("Host", "", "host-token");
    TestClient intruder("Intruder", "wrong");
    TestClient guest("Guest", "secret");
    TestClient late("Late", "secret");
    std::vector<TestClient*> all{&host, &intruder, &guest, &late};

    // The host's own game joins within the process (no network, as in the game) with its token, no password.
    ISteamNetworkingSockets* pSockets = nullptr;
    HSteamNetConnection connection = k_HSteamNetConnection_Invalid;
    REQUIRE(HostSession::OpenLocalConnection(pSockets, connection));
    REQUIRE(host.Adopt(pSockets, connection));
    REQUIRE(Pump(all, [&] { return host.Accepted.has_value(); }));
    CHECK(*host.Accepted);
    CHECK(host.HostName == "Host");

    // A wrong password is refused, with the reason.
    intruder.Connect(Address());
    REQUIRE(Pump(all, [&] { return intruder.Accepted.has_value(); }));
    CHECK_FALSE(*intruder.Accepted);
    CHECK(intruder.Error.find("Wrong password") != std::string::npos);

    // The host has reported its world before the guest arrives: the guest gets it on joining.
    client::ReportWorldState report;
    report.set_game_time(12 * 3600);
    report.set_weather(0x1234);
    host.SendMessage(report);
    REQUIRE(Pump(all, [] { return false; }) == false); // let it arrive

    guest.Connect(Address());
    REQUIRE(Pump(all, [&] { return guest.Accepted.has_value() && guest.WorldState.has_value(); }));
    CHECK(*guest.Accepted);
    CHECK(guest.HostName == "Host");
    CHECK(guest.WorldState->first == 12 * 3600);
    CHECK(guest.WorldState->second == 0x1234);
    REQUIRE(Pump(all, [&] { return host.Joined.size() == 1; }));
    CHECK(host.Joined[0] == "Guest");
    CHECK(guest.Joined.empty());

    // Two players is the limit here.
    late.Connect(Address());
    REQUIRE(Pump(all, [&] { return late.Accepted.has_value(); }));
    CHECK_FALSE(*late.Accepted);
    CHECK(late.Error.find("full") != std::string::npos);

    // Only the host's world counts: a guest's report changes nothing.
    client::ReportWorldState fake;
    fake.set_game_time(1);
    fake.set_weather(1);
    guest.SendMessage(fake);
    report.set_game_time(13 * 3600);
    host.SendMessage(report);
    REQUIRE(Pump(all, [&] { return guest.WorldState->first == 13 * 3600; }));
    CHECK(guest.WorldState->second == 0x1234);

    // A guest leaving is announced to the others.
    guest.Close();
    REQUIRE(Pump(all, [&] { return host.Left.size() == 1; }));
    CHECK(host.Left[0] == "Guest");

    // Ending the session disconnects everyone still in it.
    TestClient again("Again", "secret");
    all.push_back(&again);
    again.Connect(Address());
    REQUIRE(Pump(all, [&] { return again.Accepted.has_value(); }));
    CHECK(*again.Accepted);

    HostSession::Stop();
    CHECK_FALSE(HostSession::IsRunning());
    REQUIRE(Pump(all, [&] { return again.Disconnected && host.Disconnected; }));
}

TEST_CASE("A taken port moves the session to the next one")
{
    HostSession::Settings settings;
    settings.Port = kPort + 10;
    REQUIRE(HostSession::Start(settings));
    const auto first = HostSession::GetPort();

    // Something else already listens on the first port.
    struct Blocker final : Server
    {
        Blocker() : Server(client::kIdentifier, server::kIdentifier) {}
        void OnUpdate() override {}
        void OnConsume(const void*, uint32_t, ConnectionId) override {}
        void OnConnection(ConnectionId) override {}
        void OnDisconnection(ConnectionId, EDisconnectReason) override {}
    } blocker;
    HostSession::Stop();
    // An ended session keeps its port for a second so its players learn it ended.
    REQUIRE(Pump({}, [] { return false; }) == false);
    REQUIRE(blocker.Host(first, 10));

    REQUIRE(HostSession::Start(settings));
    CHECK(HostSession::GetPort() == first + 1);
    HostSession::Stop();
}
