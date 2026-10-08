// SessionRunner: sessions on a dedicated network thread, with the game touched only from the main thread.
// Real GameNetworkingSockets connections on localhost (ports 27190-27199).

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>

#include "Test.hpp"
#include "client/SessionRunner.hpp"
#include "core/Crypto.hpp"
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

// Wraps a SimPlayer as "the game": counts calls and records any call made off the main (test) thread.
class GameProbe final : public IGameAdapter
{
public:
    GameProbe(const IClock& aClock, sim::SimPlayerConfig aConfig)
        : sim(aClock, std::move(aConfig))
        , mainThread(std::this_thread::get_id())
    {
    }

    bool CaptureLocal(LocalSample& aOut) override
    {
        Check();
        return sim.CaptureLocal(aOut);
    }
    LocalAppearance GetLocalAppearance() override
    {
        Check();
        return appearanceOverride ? *appearanceOverride : sim.GetLocalAppearance();
    }
    void OnRemotePlayerJoined(const RemotePlayerInfo& aInfo) override
    {
        Check();
        sim.OnRemotePlayerJoined(aInfo);
    }
    void OnRemoteAppearance(PeerId aPeer, const LocalAppearance& aAppearance) override
    {
        Check();
        ++appearancesReceived;
        sim.OnRemoteAppearance(aPeer, aAppearance);
    }
    void OnRemotePlayerLeft(PeerId aPeer) override
    {
        Check();
        ++leftEvents;
        sim.OnRemotePlayerLeft(aPeer);
    }
    void DriveRemotePlayer(PeerId aPeer, const RemotePose& aPose) override
    {
        Check();
        ++drives;
        sim.DriveRemotePlayer(aPeer, aPose);
    }
    void OnStatus(const std::string& aText) override
    {
        Check();
        sim.OnStatus(aText);
    }

    sim::SimPlayer sim;
    std::thread::id mainThread;
    int wrongThreadCalls = 0;
    int appearancesReceived = 0;
    int leftEvents = 0;
    uint64_t drives = 0;
    std::optional<LocalAppearance> appearanceOverride;

private:
    void Check()
    {
        if (std::this_thread::get_id() != mainThread)
            ++wrongThreadCalls;
    }
};

sim::SimPlayerConfig Bot(const std::string& aName, float aX)
{
    sim::SimPlayerConfig config;
    config.name = aName;
    config.script = sim::Script::Circle;
    config.center = Vec3{aX, 200.0f, 10.0f};
    config.radius = 4.0f;
    config.verbose = false;
    return config;
}

HostConfig MakeHostConfig(uint16_t aPort)
{
    HostConfig config;
    config.port = aPort;
    config.gameBuild = kBuild;
    config.exeSize = 1;
    config.passwordIterations = 1000;
    return config;
}

ClientConfig MakeClientConfig(const std::string& aName)
{
    ClientConfig config;
    config.displayName = aName;
    config.gameBuild = kBuild;
    config.exeSize = 1;
    config.clientId = RandomUuid();
    return config;
}

struct Machine
{
    Machine(const IClock& aClock, const std::string& aName, float aX)
        : runner(aClock)
        , game(aClock, Bot(aName, aX))
        , name(aName)
    {
    }

    SessionRunner runner;
    GameProbe game;
    std::string name;
    bool pumping = true;
};

// Plays the role of each game's main thread: pumps every machine at ~60 Hz until aDone or the timeout.
template<typename Predicate>
bool RunUntil(const std::vector<Machine*>& aMachines, Predicate aDone, double aSeconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(aSeconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (auto* machine : aMachines)
        {
            if (machine->pumping)
                machine->runner.Pump(machine->game);
        }
        if (aDone())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return false;
}
} // namespace

TEST_CASE("runner: sessions run on the network thread, the game is only called from the main thread")
{
    SteadyClock clock;
    Machine host(clock, "Host", 100.0f);
    Machine a(clock, "Jackie", 110.0f);
    Machine b(clock, "Panam", 120.0f);

    std::string error;
    REQUIRE(host.runner.Host(MakeHostConfig(27190), MakeClientConfig("Host"), error));
    REQUIRE(a.runner.Join("127.0.0.1:27190", MakeClientConfig("Jackie"), error));
    REQUIRE(b.runner.Join("127.0.0.1:27190", MakeClientConfig("Panam"), error));

    const std::vector<Machine*> all{&host, &a, &b};
    const bool everyoneSeesEveryone = RunUntil(
        all,
        [&]
        {
            for (auto* machine : all)
            {
                if (machine->game.sim.KnownPoses().size() != 2)
                    return false;
            }
            return true;
        },
        8.0);
    REQUIRE(everyoneSeesEveryone);

    // Let interpolation settle, then check what one client sees of the host player.
    RunUntil(all, [] { return false; }, 1.0);
    const float error2 = Distance(a.game.sim.KnownPoses().at(kHostPeer).position, host.game.sim.Position());
    CHECK(error2 < 1.5f);

    const auto status = host.runner.GetStatus();
    CHECK(status.hosting);
    CHECK_EQ(status.hostPlayers, 3u);
    CHECK(status.netTicks > 100);
    CHECK(host.runner.StatusText().find("hosting, 3 player(s)") != std::string::npos);

    for (auto* machine : all)
    {
        CHECK_EQ(machine->game.wrongThreadCalls, 0);
        CHECK(machine->game.drives > 0);
    }
}

TEST_CASE("runner: a frozen main thread (loading screen) keeps the session alive")
{
    SteadyClock clock;
    Machine host(clock, "Host", 100.0f);
    Machine loader(clock, "Loader", 110.0f);

    auto config = MakeHostConfig(27191);
    config.peerTimeoutUs = 1'500'000; // short, so the test is quick: 1.5 s without traffic = dropped

    std::string error;
    REQUIRE(host.runner.Host(config, MakeClientConfig("Host"), error));
    REQUIRE(loader.runner.Join("127.0.0.1:27191", MakeClientConfig("Loader"), error));

    const std::vector<Machine*> both{&host, &loader};
    REQUIRE(RunUntil(both, [&] { return host.game.sim.KnownPoses().size() == 1; }, 8.0));

    // "Loading screen": the loader's main thread stops pumping for twice the timeout.
    loader.pumping = false;
    RunUntil(both, [] { return false; }, 3.0);

    CHECK_EQ(host.runner.GetStatus().hostPlayers, 2u);
    CHECK(loader.runner.GetStatus().clientState == ClientState::Joined);
    CHECK(loader.runner.GetStatus().longestMainThreadGapUs >= 2'900'000);
    CHECK_EQ(host.game.leftEvents, 0);

    // After the load, its movement reaches the host again.
    loader.pumping = true;
    const auto drivesBefore = host.game.drives;
    const Vec3 before = host.game.sim.KnownPoses().at(1).position;
    REQUIRE(RunUntil(both, [&] { return Distance(host.game.sim.KnownPoses().at(1).position, before) > 0.5f; }, 3.0));
    CHECK(host.game.drives > drivesBefore);
}

TEST_CASE("runner: without the network thread the same freeze would drop the player (control)")
{
    // Same timeout, but the client's session is ticked by the "main thread" itself (the M0a design).
    SteadyClock clock;
    Machine host(clock, "Host", 100.0f);

    auto config = MakeHostConfig(27192);
    config.peerTimeoutUs = 1'500'000;
    std::string error;
    REQUIRE(host.runner.Host(config, MakeClientConfig("Host"), error));

    GnsTransport::InitLibrary(error);
    {
        GnsTransport transport;
        sim::SimPlayer player(clock, Bot("OldStyle", 110.0f));
        ClientSession session(transport, player, clock, MakeClientConfig("OldStyle"));
        REQUIRE(session.Connect("127.0.0.1:27192", error));

        const std::vector<Machine*> hostOnly{&host};
        REQUIRE(RunUntil(
            hostOnly,
            [&]
            {
                session.Tick();
                return host.runner.GetStatus().hostPlayers == 2;
            },
            8.0));

        // Frozen for 3 s: nobody ticks the session.
        RunUntil(hostOnly, [] { return false; }, 3.0);
        CHECK_EQ(host.runner.GetStatus().hostPlayers, 1u);
    }
    GnsTransport::ShutdownLibrary(false);
}

TEST_CASE("runner: appearance changes are re-sent; leave and rejoin")
{
    SteadyClock clock;
    Machine host(clock, "Host", 100.0f);
    Machine client(clock, "Judy", 110.0f);

    std::string error;
    REQUIRE(host.runner.Host(MakeHostConfig(27193), MakeClientConfig("Host"), error));
    REQUIRE(client.runner.Join("127.0.0.1:27193", MakeClientConfig("Judy"), error));

    const std::vector<Machine*> both{&host, &client};
    REQUIRE(RunUntil(both, [&] { return host.game.appearancesReceived >= 1; }, 8.0));
    CHECK_EQ(host.game.sim.KnownAppearances().at(1).bodyGender, 0);

    // The client's character changes (e.g. new clothes); the host gets the new appearance within ~1 s.
    LocalAppearance changed;
    changed.bodyGender = 1;
    changed.equipment = {"Items.Jacket_01"};
    client.game.appearanceOverride = changed;
    REQUIRE(RunUntil(both, [&] { return host.game.sim.KnownAppearances().at(1).bodyGender == 1; }, 4.0));
    CHECK(host.game.sim.KnownAppearances().at(1).equipment == std::vector<std::string>{"Items.Jacket_01"});

    // Leave: the host sees the player go; the client is idle.
    client.runner.Leave("test leave");
    CHECK(!client.runner.IsActive());
    REQUIRE(RunUntil(both, [&] { return host.game.leftEvents == 1; }, 5.0));
    CHECK_EQ(host.runner.GetStatus().hostPlayers, 1u);

    // Rejoin with the same runner.
    REQUIRE(client.runner.Join("127.0.0.1:27193", MakeClientConfig("Judy"), error));
    REQUIRE(RunUntil(both, [&] { return host.runner.GetStatus().hostPlayers == 2; }, 8.0));

    // A client whose host goes away is told so right away (not after a connection timeout) and cleans up.
    host.runner.Leave("host quits");
    REQUIRE(RunUntil(both, [&] { return !client.runner.IsActive(); }, 3.0));
    CHECK(client.runner.LastStatus().find("removed from session") != std::string::npos);
    CHECK_EQ(client.game.wrongThreadCalls, 0);
    CHECK_EQ(host.game.wrongThreadCalls, 0);
}
