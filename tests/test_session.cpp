// End-to-end tests over real GameNetworkingSockets connections on localhost: a host with its own player
// (in-process pair) and remote clients over UDP, all driven by scripted SimPlayers.

#include <chrono>
#include <memory>
#include <set>
#include <thread>

#include "Test.hpp"
#include "client/ClientSession.hpp"
#include "core/Crypto.hpp"
#include "host/HostService.hpp"
#include "net/GnsTransport.hpp"
#include "tools/sim/SimPlayer.hpp"

using namespace coop;

namespace
{
constexpr const char* kBuild = "test-build";

Uuid RandomUuid()
{
    Uuid id{};
    crypto::RandomBytes(id.data(), id.size());
    return id;
}

struct Player
{
    std::unique_ptr<GnsTransport> transport;
    std::unique_ptr<sim::SimPlayer> sim;
    std::unique_ptr<ClientSession> session;

    // The session closes its connection through the transport, so it must go first.
    void Reset()
    {
        session.reset();
        sim.reset();
        transport.reset();
    }
};

struct Harness
{
    SteadyClock clock;
    std::unique_ptr<GnsTransport> hostTransport;
    std::unique_ptr<HostService> host;
    Player hostPlayer;
    std::vector<Player> clients;
    uint16_t port = 0;

    explicit Harness(uint16_t aPort, int aMaxPlayers = kMaxPlayers, const std::string& aPassword = "pw")
        : port(aPort)
    {
        std::string error;
        GnsTransport::InitLibrary(error);

        HostConfig config;
        config.port = aPort;
        config.password = aPassword;
        config.gameBuild = kBuild;
        config.exeSize = 1;
        config.maxPlayers = aMaxPlayers;
        config.passwordIterations = 1000; // fast for tests

        hostTransport = std::make_unique<GnsTransport>();
        host = std::make_unique<HostService>(*hostTransport, clock, config);
        started = host->Start(error);

        sim::SimPlayerConfig botConfig;
        botConfig.name = "Host";
        botConfig.script = sim::Script::Circle;
        botConfig.center = Vec3{100.0f, 200.0f, 10.0f};
        botConfig.radius = 5.0f;
        botConfig.verbose = false;
        hostPlayer = MakePlayer(botConfig, "", kBuild);

        ConnId hostSide = kInvalidConn;
        ConnId playerSide = kInvalidConn;
        pairCreated = GnsTransport::CreatePair(*hostTransport, hostSide, *hostPlayer.transport, playerSide);
        host->MarkLocal(hostSide);
        host->AdoptConnection(hostSide);
        hostPlayer.session->UseConnection(playerSide);
    }

    ~Harness()
    {
        for (auto& client : clients)
            client.Reset();
        clients.clear();
        hostPlayer.Reset();
        host.reset();
        hostTransport.reset();
        GnsTransport::ShutdownLibrary(false);
    }

    Player MakePlayer(const sim::SimPlayerConfig& aConfig, const std::string& aPassword, const std::string& aBuild)
    {
        Player player;
        player.transport = std::make_unique<GnsTransport>();
        player.sim = std::make_unique<sim::SimPlayer>(clock, aConfig);
        ClientConfig config;
        config.displayName = aConfig.name;
        config.password = aPassword;
        config.gameBuild = aBuild;
        config.exeSize = 1;
        config.clientId = RandomUuid();
        player.session = std::make_unique<ClientSession>(*player.transport, *player.sim, clock, config);
        return player;
    }

    Player& AddClient(const std::string& aName, const std::string& aPassword = "pw", const std::string& aBuild = kBuild,
                      bool aFemale = false)
    {
        sim::SimPlayerConfig config;
        config.name = aName;
        config.script = sim::Script::Line;
        config.center = Vec3{110.0f + 3.0f * static_cast<float>(clients.size()), 200.0f, 10.0f};
        config.radius = 4.0f;
        config.female = aFemale;
        config.verbose = false;
        clients.push_back(MakePlayer(config, aPassword, aBuild));
        std::string error;
        clients.back().session->Connect("127.0.0.1:" + std::to_string(port), error);
        return clients.back();
    }

    void Tick()
    {
        host->Tick();
        hostPlayer.session->Tick();
        for (auto& client : clients)
            client.session->Tick();
    }

    template<typename Predicate>
    bool RunUntil(Predicate aDone, double aTimeoutSeconds)
    {
        const auto deadline = clock.NowUs() + FromSeconds(aTimeoutSeconds);
        while (clock.NowUs() < deadline)
        {
            Tick();
            if (aDone())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    bool started = false;
    bool pairCreated = false;
};
} // namespace

TEST_CASE("session: host player and three clients see each other")
{
    Harness h(27177);
    REQUIRE(h.started);
    REQUIRE(h.pairCreated);

    h.AddClient("Jackie");
    h.AddClient("Panam", "pw", kBuild, true);
    h.AddClient("Judy", "pw", kBuild, true);

    const bool everyoneSeesEveryone = h.RunUntil(
        [&]
        {
            if (h.hostPlayer.sim->KnownPoses().size() != 3)
                return false;
            for (const auto& client : h.clients)
            {
                if (client.sim->KnownPoses().size() != 3)
                    return false;
            }
            return true;
        },
        8.0);
    REQUIRE(everyoneSeesEveryone);

    CHECK_EQ(h.host->JoinedCount(), 4u);
    CHECK_EQ(h.hostPlayer.session->LocalPeer(), kHostPeer);

    std::set<PeerId> peers;
    for (const auto& client : h.clients)
    {
        CHECK(client.session->State() == ClientState::Joined);
        peers.insert(client.session->LocalPeer());
    }
    CHECK(peers == std::set<PeerId>({1, 2, 3}));

    // Let interpolation settle, then compare what a client sees of the host player with the real position.
    h.RunUntil([] { return false; }, 1.0);
    const auto& seen = h.clients[0].sim->KnownPoses().at(kHostPeer);
    const float error = Distance(seen.position, h.hostPlayer.sim->Position());
    // The host player walks at 2.5 m/s and is rendered ~70-100 ms in the past.
    CHECK(error < 1.0f);

    // Appearance arrives for late joiners too.
    CHECK_EQ(h.hostPlayer.sim->KnownAppearances().size(), 3u);
    CHECK_EQ(h.clients[0].sim->KnownAppearances().at(h.clients[1].session->LocalPeer()).bodyGender, 1);

    // Clock sync: every client agrees on session time within a few milliseconds of each other. With every CPU core
    // busy (CI runners, sanitizers) the host's thread answers late on every exchange, which skews all clients' estimates
    // the same way by ~10 ms (NTP-style sync can't see one-way delays). Interpolation runs ~100 ms behind, so 15 ms is
    // still well within what the game needs.
    const TimeUs reference = h.hostPlayer.session->SessionNow();
    for (const auto& client : h.clients)
        CHECK_NEAR(static_cast<double>(client.session->SessionNow()), static_cast<double>(reference), 15'000.0);

    CHECK(h.host->Stats().statesRelayed > 100);
}

TEST_CASE("session: wrong password is rejected")
{
    Harness h(27178);
    REQUIRE(h.started);
    auto& intruder = h.AddClient("Intruder", "wrong");
    const bool closed = h.RunUntil([&] { return intruder.session->State() == ClientState::Closed; }, 6.0);
    REQUIRE(closed);
    CHECK(intruder.session->LastError().find("wrong password") != std::string::npos);
    CHECK_EQ(h.host->JoinedCount(), 1u);
}

TEST_CASE("session: different game build is rejected")
{
    Harness h(27179);
    REQUIRE(h.started);
    auto& other = h.AddClient("Patched", "pw", "9.9.9.9");
    const bool closed = h.RunUntil([&] { return other.session->State() == ClientState::Closed; }, 6.0);
    REQUIRE(closed);
    CHECK(other.session->LastError().find("game build") != std::string::npos);
}

TEST_CASE("session: full session rejects extra players")
{
    Harness h(27180, 2);
    REQUIRE(h.started);
    auto& first = h.AddClient("First");
    REQUIRE(h.RunUntil([&] { return first.session->State() == ClientState::Joined; }, 6.0));
    auto& second = h.AddClient("Second");
    const bool closed = h.RunUntil([&] { return second.session->State() == ClientState::Closed; }, 6.0);
    REQUIRE(closed);
    CHECK(second.session->LastError().find("full") != std::string::npos);
}

TEST_CASE("session: leaving removes the puppet everywhere")
{
    Harness h(27181);
    REQUIRE(h.started);
    h.AddClient("Stays");
    auto& leaver = h.AddClient("Leaves");
    REQUIRE(h.RunUntil([&] { return h.clients[0].sim->KnownPoses().size() == 2; }, 8.0));

    const PeerId leaverPeer = leaver.session->LocalPeer();
    leaver.session->Disconnect("bye");
    const bool removed = h.RunUntil(
        [&]
        {
            return h.clients[0].sim->KnownPoses().count(leaverPeer) == 0
                && h.hostPlayer.sim->KnownPoses().count(leaverPeer) == 0;
        },
        6.0);
    CHECK(removed);
    CHECK_EQ(h.host->JoinedCount(), 2u);
}
