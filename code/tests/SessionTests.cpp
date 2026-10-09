#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <algorithm>
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
        m_dispatcher.sink<PacketEvent<server::SpawnCharacterResponse>>().connect<&TestClient::OnSpawned>(this);
        m_dispatcher.sink<PacketEvent<server::NotifyCharacterLoad>>().connect<&TestClient::OnCharacterLoad>(this);
        m_dispatcher.sink<PacketEvent<server::NotifyEntityMove>>().connect<&TestClient::OnEntityMove>(this);
    }

    // What the game sends once it's in: its character, where it stands, what it wears and looks like.
    void SpawnCharacter(float aX, const std::vector<uint64_t>& acEquipment, const std::vector<uint8_t>& acCcstate)
    {
        client::SpawnCharacterRequest request;
        common::Vector3 position;
        position.set_x(aX);
        request.set_position(position);
        request.set_is_player(true);
        Vector<uint64_t> equipment(acEquipment.begin(), acEquipment.end());
        request.set_equipment(equipment);
        Vector<uint8_t> ccstate(acCcstate.begin(), acCcstate.end());
        request.set_ccstate(ccstate);
        SendMessage(request);
    }

    void Move(float aX, uint64_t aTick)
    {
        client::MoveEntityRequest request;
        request.set_id(*OwnCharacter);
        common::Vector3 position;
        position.set_x(aX);
        request.set_position(position);
        request.set_tick(aTick);
        SendMessage(request);
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
    void OnSpawned(const PacketEvent<server::SpawnCharacterResponse>& acEvent)
    {
        if (acEvent.has_id())
            OwnCharacter = acEvent.get_id();
    }
    void OnCharacterLoad(const PacketEvent<server::NotifyCharacterLoad>& acEvent)
    {
        const auto& equipment = acEvent.get_equipment();
        const auto& ccstate = acEvent.get_ccstate();
        Characters.push_back({acEvent.get_id(), acEvent.get_position().get_x(),
                              std::vector<uint64_t>(equipment.begin(), equipment.end()),
                              std::vector<uint8_t>(ccstate.begin(), ccstate.end())});
    }
    void OnEntityMove(const PacketEvent<server::NotifyEntityMove>& acEvent)
    {
        Moves.emplace_back(acEvent.get_id(), acEvent.get_position().get_x());
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

    struct Character
    {
        uint64_t Id;
        float X;
        std::vector<uint64_t> Equipment;
        std::vector<uint8_t> Ccstate;
    };
    std::optional<uint64_t> OwnCharacter;
    std::vector<Character> Characters; // the others', as the session loads them
    std::vector<std::pair<uint64_t, float>> Moves;

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
    HostSession::Shutdown();
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
    HostSession::Shutdown();
}

TEST_CASE("Players get each other's characters with their items and look, then their moves")
{
    HostSession::Settings settings;
    settings.Port = kPort + 20;
    settings.Password = "secret";
    settings.HostToken = "host-token";
    REQUIRE(HostSession::Start(settings));

    TestClient host("Host", "", "host-token");
    TestClient guest("Guest", "secret");
    std::vector<TestClient*> all{&host, &guest};

    ISteamNetworkingSockets* pSockets = nullptr;
    HSteamNetConnection connection = k_HSteamNetConnection_Invalid;
    REQUIRE(HostSession::OpenLocalConnection(pSockets, connection));
    REQUIRE(host.Adopt(pSockets, connection));
    guest.Connect("127.0.0.1:" + std::to_string(HostSession::GetPort()));
    REQUIRE(Pump(all, [&] { return host.Accepted.has_value() && guest.Accepted.has_value(); }));
    REQUIRE(*host.Accepted);
    REQUIRE(*guest.Accepted);

    // Item ids as TweakDBIDs (name hash and length), not names: the game has none outside debug builds.
    const std::vector<uint64_t> hostItems{0x12'3456'789Aull, 0xFF'FFFF'FFFFull};
    const std::vector<uint64_t> guestItems{0x07'CAFE'BABEull};
    host.SpawnCharacter(1.f, hostItems, {1, 2, 3, 4});
    REQUIRE(Pump(all, [&] { return host.OwnCharacter.has_value(); }));
    guest.SpawnCharacter(2.f, guestItems, {9, 9});
    REQUIRE(Pump(all, [&] { return guest.OwnCharacter.has_value() && !host.Characters.empty() && !guest.Characters.empty(); }));

    // Each gets the other's character, exactly as sent, and not its own.
    REQUIRE(host.Characters.size() == 1);
    CHECK(host.Characters[0].Id == *guest.OwnCharacter);
    CHECK(host.Characters[0].X == 2.f);
    CHECK(host.Characters[0].Equipment == guestItems);
    CHECK(host.Characters[0].Ccstate == std::vector<uint8_t>{9, 9});
    REQUIRE(guest.Characters.size() == 1);
    CHECK(guest.Characters[0].Id == *host.OwnCharacter);
    CHECK(guest.Characters[0].Equipment == hostItems);
    CHECK(guest.Characters[0].Ccstate == std::vector<uint8_t>{1, 2, 3, 4});

    // The games create these under the same ids in their own worlds, which keep flecs' built-in entities and
    // components below and their own entities from 10'000'000 up.
    for (const auto id : {*host.OwnCharacter, *guest.OwnCharacter})
    {
        CHECK((id & 0xFFFF'FFFFull) >= 1'000'000);
        CHECK((id & 0xFFFF'FFFFull) < 10'000'000);
    }

    // A newcomer moves right away; the host gets it (and has to cope with its character still spawning).
    guest.Move(3.f, 1);
    REQUIRE(Pump(all, [&] {
        return std::any_of(host.Moves.begin(), host.Moves.end(),
                           [&](const auto& acMove) { return acMove.first == *guest.OwnCharacter && acMove.second == 3.f; });
    }));

    HostSession::Shutdown();
}
