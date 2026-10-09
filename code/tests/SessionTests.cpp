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
        m_dispatcher.sink<PacketEvent<server::NotifyCharacterState>>().connect<&TestClient::OnCharacterState>(this);
        m_dispatcher.sink<PacketEvent<server::NotifyCharacterShot>>().connect<&TestClient::OnCharacterShot>(this);
        m_dispatcher.sink<PacketEvent<server::NotifyVehicleLoad>>().connect<&TestClient::OnVehicleLoad>(this);
        m_dispatcher.sink<PacketEvent<server::NotifyVehicleEnter>>().connect<&TestClient::OnVehicleEnter>(this);
        m_dispatcher.sink<PacketEvent<server::NotifyVehicleExit>>().connect<&TestClient::OnVehicleExit>(this);
        m_dispatcher.sink<PacketEvent<server::NotifyVehicleControlAssigned>>().connect<&TestClient::OnVehicleControl>(this);
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

    void Move(float aX, uint64_t aTick, float aAimPitch = 0.f)
    {
        client::MoveEntityRequest request;
        request.set_id(*OwnCharacter);
        common::Vector3 position;
        position.set_x(aX);
        request.set_position(position);
        request.set_tick(aTick);
        request.set_aim_pitch(aAimPitch);
        SendMessage(request);
    }

    void SendState(uint64_t aId, uint32_t aLocomotion, uint64_t aWeapon)
    {
        client::CharacterStateRequest request;
        request.set_id(aId);
        request.set_locomotion(aLocomotion);
        request.set_upper_body(6);
        request.set_weapon_state(5);
        request.set_weapon(aWeapon);
        SendMessage(request);
    }

    void Shoot(uint32_t aCount)
    {
        client::CharacterShotRequest request;
        request.set_id(*OwnCharacter);
        request.set_count(aCount);
        SendMessage(request);
    }

    // Into a vehicle the session doesn't know yet (aKnown 0), or one it does, in the seat (CName hash).
    void EnterVehicle(uint64_t aKnown, uint64_t aSeat)
    {
        client::EnterVehicleRequest request;
        request.set_id(*OwnCharacter);
        request.set_vehicle_id(0x0B'5EED'CAFEull);
        request.set_sit_id(aSeat);
        if (aKnown)
            request.set_remote_vehicle_id(aKnown);
        SendMessage(request);
    }

    void ExitVehicle()
    {
        client::ExitVehicleRequest request;
        request.set_id(*OwnCharacter);
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
        Moves.push_back({acEvent.get_id(), acEvent.get_position().get_x(), acEvent.get_aim_pitch()});
    }
    void OnCharacterState(const PacketEvent<server::NotifyCharacterState>& acEvent)
    {
        // A state only makes sense for a character the session loaded here before.
        const bool loaded = std::any_of(Characters.begin(), Characters.end(),
                                        [&](const Character& acCharacter) { return acCharacter.Id == acEvent.get_id(); });
        States.push_back({acEvent.get_id(), acEvent.get_locomotion(), acEvent.get_upper_body(), acEvent.get_weapon_state(),
                          acEvent.get_weapon(), loaded});
    }
    void OnVehicleLoad(const PacketEvent<server::NotifyVehicleLoad>& acEvent) { VehicleLoads.push_back(acEvent.get_id()); }
    void OnVehicleEnter(const PacketEvent<server::NotifyVehicleEnter>& acEvent)
    {
        VehicleEnters.emplace_back(acEvent.get_character_id(), acEvent.get_vehicle_id());
    }
    void OnVehicleExit(const PacketEvent<server::NotifyVehicleExit>& acEvent) { VehicleExits.push_back(acEvent.get_character_id()); }
    void OnVehicleControl(const PacketEvent<server::NotifyVehicleControlAssigned>& acEvent)
    {
        Driving.push_back(acEvent.get_vehicle_id());
    }
    void OnCharacterShot(const PacketEvent<server::NotifyCharacterShot>& acEvent)
    {
        Shots.push_back({acEvent.get_id(), acEvent.get_count()});
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
    struct MoveSeen
    {
        uint64_t Id;
        float X;
        float AimPitch;
    };
    std::vector<MoveSeen> Moves;

    struct State
    {
        uint64_t Id;
        uint32_t Locomotion;
        uint32_t UpperBody;
        uint32_t WeaponState;
        uint64_t Weapon;
        bool AfterLoad;
    };
    std::vector<State> States;

    struct Shot
    {
        uint64_t Id;
        uint32_t Count;
    };
    std::vector<Shot> Shots;

    std::vector<uint64_t> VehicleLoads;
    std::vector<std::pair<uint64_t, uint64_t>> VehicleEnters; // character, vehicle
    std::vector<uint64_t> VehicleExits;
    std::vector<uint64_t> Driving; // the vehicles the session made this player the driver of

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

    // A newcomer moves right away; the host gets it (and has to cope with its character still spawning), with where
    // the guest aims.
    guest.Move(3.f, 1, 0.25f);
    REQUIRE(Pump(all, [&] {
        return std::any_of(host.Moves.begin(), host.Moves.end(),
                           [&](const auto& acMove) { return acMove.Id == *guest.OwnCharacter && acMove.X == 3.f; });
    }));
    const auto move = std::find_if(host.Moves.begin(), host.Moves.end(),
                                   [&](const auto& acMove) { return acMove.Id == *guest.OwnCharacter && acMove.X == 3.f; });
    CHECK(move->AimPitch == 0.25f);

    HostSession::Shutdown();
}

TEST_CASE("Players see each other's stance and weapon, also when they arrive later, and each other's shots")
{
    HostSession::Settings settings;
    settings.Port = kPort + 30;
    settings.Password = "secret";
    settings.HostToken = "host-token";
    REQUIRE(HostSession::Start(settings));

    TestClient host("Host", "", "host-token");
    TestClient guest("Guest", "secret");
    TestClient late("Late", "secret");
    std::vector<TestClient*> all{&host, &guest, &late};

    ISteamNetworkingSockets* pSockets = nullptr;
    HSteamNetConnection connection = k_HSteamNetConnection_Invalid;
    REQUIRE(HostSession::OpenLocalConnection(pSockets, connection));
    REQUIRE(host.Adopt(pSockets, connection));
    guest.Connect("127.0.0.1:" + std::to_string(HostSession::GetPort()));
    REQUIRE(Pump(all, [&] { return host.Accepted.has_value() && guest.Accepted.has_value(); }));

    host.SpawnCharacter(1.f, {}, {});
    guest.SpawnCharacter(2.f, {}, {});
    REQUIRE(Pump(all, [&] {
        return host.OwnCharacter && guest.OwnCharacter && !host.Characters.empty() && !guest.Characters.empty();
    }));
    const auto hostCharacter = *host.OwnCharacter;

    // The host crouches with a pistol in hand: the guest's game gets it, the host's own doesn't.
    constexpr uint64_t kPistol = 0x14'0D8C'2A51ull;
    host.SendState(hostCharacter, 1, kPistol);
    REQUIRE(Pump(all, [&] { return !guest.States.empty(); }));
    CHECK(guest.States[0].Id == hostCharacter);
    CHECK(guest.States[0].Locomotion == 1);
    CHECK(guest.States[0].UpperBody == 6);
    CHECK(guest.States[0].WeaponState == 5);
    CHECK(guest.States[0].Weapon == kPistol);

    // Nobody changes another player's character.
    guest.SendState(hostCharacter, 99, 0);
    guest.SendState(0, 99, 0);
    guest.SendState(0xFFFF'FFFFull, 99, 0);

    // Who arrives later gets the character, then its latest state.
    late.Connect("127.0.0.1:" + std::to_string(HostSession::GetPort()));
    REQUIRE(Pump(all, [&] { return late.Accepted.has_value(); }));
    REQUIRE(*late.Accepted);
    late.SpawnCharacter(3.f, {}, {});
    REQUIRE(Pump(all, [&] {
        return std::any_of(late.States.begin(), late.States.end(), [&](const auto& acState) { return acState.Id == hostCharacter; });
    }));
    const auto state = std::find_if(late.States.begin(), late.States.end(), [&](const auto& acState) { return acState.Id == hostCharacter; });
    CHECK(state->AfterLoad);
    CHECK(state->Locomotion == 1);
    CHECK(state->Weapon == kPistol);
    CHECK(guest.States.size() == 1);
    CHECK(host.States.empty());

    // Shots go to the others as they come, a burst at most.
    host.Shoot(3);
    host.Shoot(1000);
    REQUIRE(Pump(all, [&] { return guest.Shots.size() == 2 && late.Shots.size() == 2; }));
    // (unreliable: in any order)
    std::sort(guest.Shots.begin(), guest.Shots.end(), [](const auto& acA, const auto& acB) { return acA.Count < acB.Count; });
    CHECK(guest.Shots[0].Id == hostCharacter);
    CHECK(guest.Shots[0].Count == 3);
    CHECK(guest.Shots[1].Count == 30);
    CHECK(host.Shots.empty());

    HostSession::Shutdown();
}

TEST_CASE("A vehicle stays one vehicle for everyone, whoever gets in and out of it")
{
    HostSession::Settings settings;
    settings.Port = kPort + 40;
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
    host.SpawnCharacter(1.f, {}, {});
    guest.SpawnCharacter(2.f, {}, {});
    REQUIRE(Pump(all, [&] { return host.OwnCharacter && guest.OwnCharacter; }));

    constexpr uint64_t kDriver = 0xb000b1d029d0cea0ull; // seat_front_left
    constexpr uint64_t kPassenger = 0x1234ull;

    // The host drives a car of their own game: the guest's game gets it once, with the host in it, and the host is
    // its driver, under the session's id.
    host.EnterVehicle(0, kDriver);
    REQUIRE(Pump(all, [&] { return host.Driving.size() == 1 && guest.VehicleEnters.size() == 1; }));
    const auto car = host.Driving[0];
    REQUIRE(guest.VehicleLoads.size() == 1);
    CHECK(guest.VehicleLoads[0] == car);
    CHECK(guest.VehicleEnters[0] == std::make_pair(*host.OwnCharacter, car));

    // The guest rides along in their copy; the host's game seats them in the host's car.
    guest.EnterVehicle(car, kPassenger);
    REQUIRE(Pump(all, [&] { return host.VehicleEnters.size() == 1; }));
    CHECK(host.VehicleEnters[0] == std::make_pair(*guest.OwnCharacter, car));
    CHECK(guest.Driving.empty());

    // The driver gets out first, then the passenger.
    host.ExitVehicle();
    REQUIRE(Pump(all, [&] { return guest.VehicleExits.size() == 1; }));
    guest.ExitVehicle();
    REQUIRE(Pump(all, [&] { return host.VehicleExits.size() == 1; }));

    // Back in, by its id: the same car, no second one for the guest.
    host.EnterVehicle(car, kDriver);
    REQUIRE(Pump(all, [&] { return host.Driving.size() == 2 && guest.VehicleEnters.size() == 2; }));
    CHECK(host.Driving[1] == car);
    CHECK(guest.VehicleEnters[1] == std::make_pair(*host.OwnCharacter, car));
    host.ExitVehicle();
    REQUIRE(Pump(all, [&] { return guest.VehicleExits.size() == 2; }));

    // The guest takes the wheel of it: now they drive it, the host's game seats them as its driver.
    guest.EnterVehicle(car, kDriver);
    REQUIRE(Pump(all, [&] { return guest.Driving.size() == 1 && host.VehicleEnters.size() == 2; }));
    CHECK(guest.Driving[0] == car);
    CHECK(host.VehicleEnters[1] == std::make_pair(*guest.OwnCharacter, car));
    CHECK(guest.VehicleLoads.size() == 1);
    CHECK(host.VehicleLoads.empty());

    HostSession::Shutdown();
}
