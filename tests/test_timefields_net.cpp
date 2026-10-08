// Time fields over the network (docs/01-architecture.md §8, docs/02-systems.md §6.1): activations reach every
// machine, players near each other share the slow-down, the activator stays at full speed, overlapping
// activations resolve proportionally, joining and leaving a field ease in and out, and every machine computes
// the same rates. Deterministic: in-memory transport, manual clock.

#include <cmath>

#include "SimWorld.hpp"
#include "Test.hpp"
#include "core/TimeFieldClock.hpp"
#include "protocol/Codec.hpp"

using namespace coop;
using test::Machine;
using test::World;

namespace
{
constexpr TimeUs kSecond = kUsPerSecond;
constexpr int kFramesPerSecond = 60;

bool SameGroup(const World& aWorld, const Machine& aA, const Machine& aB)
{
    uint16_t groupA = 0;
    uint16_t groupB = 0;
    for (const auto& entry : aWorld.host->TimeGroups())
    {
        if (entry.peer == aA.Peer())
            groupA = entry.group;
        if (entry.peer == aB.Peer())
            groupB = entry.group;
    }
    return groupA != 0 && groupA == groupB;
}

double OffsetUs(const Machine& aMachine)
{
    const TimeUs now = aMachine.session->SessionNow();
    return aMachine.session->WorldTimeAt(now) - static_cast<double>(now);
}

// Distance a bot covers in aFrames.
float Moved(World& aWorld, Machine& aBot, int aFrames)
{
    const Vec3 before = aBot.sim->Position();
    aWorld.Step(aFrames);
    return Distance(before, aBot.sim->Position());
}
} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// The clock on its own

TEST_CASE("time clock: rates follow groups, with a smooth crossfade when the group changes")
{
    TimeFieldClock clock;
    TimeActivation sandy;
    sandy.id = 0x01000001u;
    sandy.peer = 1;
    sandy.scale = 0.25f;
    sandy.start = 1 * kSecond;
    sandy.end = 9 * kSecond;
    clock.Upsert(sandy);

    // Peer 2 is alone at first, then joins peer 1's group at 3 s.
    clock.SetGroup(1, 0, {1});
    clock.SetGroup(2, 0, {2});
    clock.SetGroup(2, 3 * kSecond, {1, 2});

    CHECK(std::abs(clock.WorldRate(1, 2 * kSecond) - 0.25f) < 1e-6f);
    CHECK(std::abs(clock.PersonalRate(1, 2 * kSecond) - 1.0f) < 1e-6f); // the activator
    CHECK(std::abs(clock.WorldRate(2, 2 * kSecond) - 1.0f) < 1e-6f);    // not in range yet

    // Crossfade over 300 ms: continuous and monotonic.
    float previous = clock.WorldRate(2, 3 * kSecond);
    CHECK(std::abs(previous - 1.0f) < 1e-6f);
    for (TimeUs t = 3 * kSecond; t <= 3 * kSecond + TimeFieldClock::kRampUs; t += 5'000)
    {
        const float rate = clock.WorldRate(2, t);
        CHECK(rate <= previous + 1e-6f);
        CHECK(previous - rate < 0.02f);
        previous = rate;
    }
    CHECK(std::abs(clock.WorldRate(2, 4 * kSecond) - 0.25f) < 1e-6f);
    CHECK(std::abs(clock.PersonalRate(2, 4 * kSecond) - 0.25f) < 1e-6f);

    // Order independence: the same facts in another order give the same answers.
    TimeFieldClock other;
    other.SetGroup(2, 3 * kSecond, {1, 2});
    other.Upsert(sandy);
    other.SetGroup(1, 0, {1});
    for (TimeUs t = 3 * kSecond + 300'000; t < 10 * kSecond; t += 250'000)
        CHECK(std::abs(other.WorldRate(2, t) - clock.WorldRate(2, t)) < 1e-6f);

    // World time: 8 s at 0.25 leaves peer 1's world 6 s behind (7.7 s held at 0.75 behind, plus half of that
    // rate over each 300 ms ease).
    const double behind = 10.0 * kSecond - clock.IntegrateWorld(1, 0, 10 * kSecond);
    CHECK(std::abs(behind - 6.0 * kSecond) < 100.0);

    // Pruning doesn't change what matters afterwards.
    const float before = clock.WorldRate(2, 8 * kSecond);
    clock.Prune(5 * kSecond);
    CHECK(std::abs(clock.WorldRate(2, 8 * kSecond) - before) < 1e-6f);
}

TEST_CASE("time fields: messages round-trip")
{
    msg::TimeFieldActivate activate;
    activate.activationId = 0x02000003u;
    activate.peer = 2;
    activate.kind = msg::TimeFieldKind::Kerenzikov;
    activate.scaleUnits = 2500;
    activate.start = 123'456'789;
    activate.end = activate.start + 2 * kSecond;
    activate.easeInMs = 150;
    activate.easeOutMs = 400;
    auto bytes = Encode(activate);
    msg::TimeFieldActivate activateOut;
    REQUIRE(Decode(bytes.data(), bytes.size(), activateOut));
    CHECK_EQ(activateOut.activationId, activate.activationId);
    CHECK(activateOut.kind == msg::TimeFieldKind::Kerenzikov);
    CHECK_EQ(activateOut.scaleUnits, 2500);
    CHECK_EQ(activateOut.end, activate.end);
    CHECK_EQ(activateOut.easeOutMs, 400);

    msg::TimeFieldMembership membership;
    membership.global = true;
    membership.entries = {{0, 1, 1000}, {1, 1, 2000}, {2, 7, 3000}};
    bytes = Encode(membership);
    msg::TimeFieldMembership membershipOut;
    REQUIRE(Decode(bytes.data(), bytes.size(), membershipOut));
    CHECK(membershipOut.global);
    CHECK(membershipOut.entries == membership.entries);
}

// ---------------------------------------------------------------------------------------------------------------------
// Sessions

TEST_CASE("time fields: a Sandevistan slows everyone nearby, the activator stays at full speed, far players are unaffected")
{
    World w;
    auto& activator = w.AddClient("Activator", sim::Script::Circle, Vec3{10.0f, 0.0f, 0.0f});
    auto& nearby = w.AddClient("Nearby", sim::Script::Line, Vec3{20.0f, 0.0f, 0.0f});
    auto& far = w.AddClient("Far", sim::Script::Line, Vec3{3000.0f, 0.0f, 0.0f});
    auto& host = w.HostPlayer();
    REQUIRE(w.RunUntil([&] { return w.Ready() && SameGroup(w, activator, nearby) && SameGroup(w, activator, host); }));
    CHECK(!SameGroup(w, activator, far));

    // Normal speed first: the Line bot walks 2.5 m/s.
    CHECK(std::abs(Moved(w, nearby, kFramesPerSecond) - 2.5f) < 0.15f);

    const uint32_t id = activator.session->ActivateTimeField(msg::TimeFieldKind::Sandevistan, 0.25f, 4 * kSecond);
    REQUIRE(id != 0);
    w.Step(kFramesPerSecond); // past the 300 ms ease-in on every machine

    for (Machine* m : {&host, &activator, &nearby})
        CHECK(std::abs(m->sim->Rates().worldRate - 0.25f) < 1e-3f);
    CHECK(std::abs(activator.sim->Rates().localRate - 1.0f) < 1e-3f);
    CHECK(activator.sim->Rates().activating);
    CHECK(std::abs(nearby.sim->Rates().localRate - 0.25f) < 1e-3f);
    CHECK(std::abs(host.sim->Rates().localRate - 0.25f) < 1e-3f);
    CHECK(std::abs(far.sim->Rates().worldRate - 1.0f) < 1e-6f);
    CHECK(std::abs(far.sim->Rates().localRate - 1.0f) < 1e-6f);

    // Every machine computes the same rates for every player at the same session time.
    const TimeUs t = host.session->SessionNow() - 200'000;
    for (const Machine* subject : {&host, &activator, &nearby, &far})
    {
        const float reference = host.session->TimeClock().WorldRate(subject->Peer(), t);
        const float personal = host.session->TimeClock().PersonalRate(subject->Peer(), t);
        for (const auto& m : w.machines)
        {
            CHECK(std::abs(m->session->TimeClock().WorldRate(subject->Peer(), t) - reference) < 1e-6f);
            CHECK(std::abs(m->session->TimeClock().PersonalRate(subject->Peer(), t) - personal) < 1e-6f);
        }
    }

    // The slow-down is real: the nearby bot covers a quarter of the ground; the activator's pace is unchanged;
    // the far bot is untouched.
    CHECK(std::abs(Moved(w, nearby, kFramesPerSecond) - 0.625f) < 0.08f);
    CHECK(Moved(w, activator, kFramesPerSecond) > 2.3f);
    CHECK(std::abs(Moved(w, far, kFramesPerSecond) - 2.5f) < 0.15f);

    // Others see the activator's puppet animate at full speed and slowed players at a quarter.
    const auto& activatorSeenByNearby = nearby.sim->KnownPoses().at(activator.Peer());
    const auto& hostSeenByNearby = nearby.sim->KnownPoses().at(host.Peer());
    CHECK(std::abs(activatorSeenByNearby.rate - 1.0f) < 1e-3f);
    CHECK(std::abs(hostSeenByNearby.rate - 0.25f) < 1e-3f);

    // After it ends everything is back to normal, and every slowed machine's world clock is behind by the same
    // amount: they simulated the same slowed stretch.
    w.Step(3 * kFramesPerSecond);
    for (const auto& m : w.machines)
        CHECK(std::abs(m->sim->Rates().worldRate - 1.0f) < 1e-6f);

    // 4 s at 0.25 with 300 ms eases: 3.7 s held (2.775 s behind) + 2 x 0.1125 s in the eases = 3.0 s.
    const double hostOffset = OffsetUs(host);
    CHECK(std::abs(hostOffset + 3.0 * kSecond) < 2'000.0);
    CHECK(std::abs(OffsetUs(activator) - hostOffset) < 2'000.0);
    CHECK(std::abs(OffsetUs(nearby) - hostOffset) < 2'000.0);
    CHECK(std::abs(OffsetUs(far)) < 1'000.0);

    // That offset travels with each player's state (for later lag compensation in world time).
    for (const auto& remote : host.session->Remotes())
    {
        if (remote.peer == nearby.Peer())
            CHECK(std::abs(remote.worldOffsetUs - OffsetUs(nearby)) < 2'000.0);
        if (remote.peer == far.Peer())
            CHECK(std::abs(remote.worldOffsetUs) < 1'000.0);
    }
}

TEST_CASE("time fields: overlapping Sandevistans resolve proportionally")
{
    World w;
    auto& strong = w.AddClient("Strong", sim::Script::Circle, Vec3{10.0f, 0.0f, 0.0f});
    auto& weak = w.AddClient("Weak", sim::Script::Line, Vec3{20.0f, 0.0f, 0.0f});
    auto& host = w.HostPlayer();
    REQUIRE(w.RunUntil([&] { return w.Ready() && SameGroup(w, strong, weak) && SameGroup(w, strong, host); }));

    strong.session->ActivateTimeField(msg::TimeFieldKind::Sandevistan, 0.25f, 6 * kSecond);
    weak.session->ActivateTimeField(msg::TimeFieldKind::Sandevistan, 0.5f, 6 * kSecond);
    w.Step(kFramesPerSecond);

    for (Machine* m : {&host, &strong, &weak})
        CHECK(std::abs(m->sim->Rates().worldRate - 0.25f) < 1e-3f); // the deepest slow-down wins
    CHECK(std::abs(strong.sim->Rates().localRate - 1.0f) < 1e-3f);  // 4x the world
    CHECK(std::abs(weak.sim->Rates().localRate - 0.5f) < 1e-3f);    // 2x the world, half the strong one
    CHECK(std::abs(host.sim->Rates().localRate - 0.25f) < 1e-3f);

    // The weaker activator really moves at half speed: 1.25 m per second instead of 2.5.
    CHECK(std::abs(Moved(w, weak, kFramesPerSecond) - 1.25f) < 0.1f);
}

TEST_CASE("time fields: cancelling, or the activator disconnecting, ends the slow-down for everyone")
{
    World w;
    auto& activator = w.AddClient("Activator", sim::Script::Circle, Vec3{10.0f, 0.0f, 0.0f});
    auto& nearby = w.AddClient("Nearby", sim::Script::Line, Vec3{20.0f, 0.0f, 0.0f});
    REQUIRE(w.RunUntil([&] { return w.Ready() && SameGroup(w, activator, nearby); }));

    const uint32_t first = activator.session->ActivateTimeField(msg::TimeFieldKind::Sandevistan, 0.25f, 8 * kSecond);
    w.Step(kFramesPerSecond);
    CHECK(nearby.sim->Rates().worldRate < 0.3f);
    activator.session->CancelTimeField(first);
    w.Step(kFramesPerSecond / 2); // 300 ms ease-out plus delivery
    CHECK(std::abs(nearby.sim->Rates().worldRate - 1.0f) < 1e-6f);
    CHECK(std::abs(activator.sim->Rates().worldRate - 1.0f) < 1e-6f);

    activator.session->ActivateTimeField(msg::TimeFieldKind::Sandevistan, 0.25f, 8 * kSecond);
    w.Step(kFramesPerSecond);
    CHECK(nearby.sim->Rates().worldRate < 0.3f);
    activator.session->Disconnect("crashed");
    w.Step(kFramesPerSecond / 2);
    CHECK(std::abs(nearby.sim->Rates().worldRate - 1.0f) < 1e-6f);
    CHECK(std::abs(w.HostPlayer().sim->Rates().worldRate - 1.0f) < 1e-6f);
}

TEST_CASE("time fields: walking into a running field eases in, walking out eases out")
{
    World w;
    auto& activator = w.AddClient("Activator", sim::Script::Idle, Vec3{10.0f, 0.0f, 0.0f});
    auto& walker = w.AddClient("Walker", sim::Script::Idle, Vec3{5000.0f, 0.0f, 0.0f});
    REQUIRE(w.RunUntil([&] { return w.Ready() && !w.host->TimeGroups().empty(); }));

    std::vector<float> rates;
    w.onFrame = [&](Machine& aMachine)
    {
        if (&aMachine == &walker)
            rates.push_back(aMachine.sim->Rates().worldRate);
    };

    activator.session->ActivateTimeField(msg::TimeFieldKind::Sandevistan, 0.25f, 10 * kSecond);
    w.Step(kFramesPerSecond);
    CHECK(std::abs(walker.sim->Rates().worldRate - 1.0f) < 1e-6f);

    // Into range: within a group update (500 ms) the walker eases down to 0.25 over 300 ms, without jumps.
    rates.clear();
    walker.sim->Teleport(Vec3{15.0f, 0.0f, 0.0f});
    w.Step(kFramesPerSecond * 3 / 2);
    REQUIRE(!rates.empty());
    CHECK(std::abs(rates.back() - 0.25f) < 1e-3f);
    float largestStep = 0.0f;
    bool monotonic = true;
    for (size_t i = 1; i < rates.size(); ++i)
    {
        largestStep = std::max(largestStep, std::abs(rates[i] - rates[i - 1]));
        monotonic = monotonic && rates[i] <= rates[i - 1] + 1e-6f;
    }
    CHECK(monotonic);
    CHECK(largestStep < 0.1f); // smoothstep over 300 ms: at most ~0.07 per frame

    // Out of range again: back up to normal speed, just as smoothly.
    rates.clear();
    walker.sim->Teleport(Vec3{5000.0f, 0.0f, 0.0f});
    w.Step(kFramesPerSecond * 3 / 2);
    CHECK(std::abs(rates.back() - 1.0f) < 1e-6f);
    largestStep = 0.0f;
    for (size_t i = 1; i < rates.size(); ++i)
        largestStep = std::max(largestStep, std::abs(rates[i] - rates[i - 1]));
    CHECK(largestStep < 0.1f);
    CHECK(activator.sim->Rates().worldRate < 0.3f); // the activator's field carries on
}

TEST_CASE("time fields: a late joiner nearby is slowed too; global scope slows everyone")
{
    {
        World w;
        auto& activator = w.AddClient("Activator", sim::Script::Idle, Vec3{10.0f, 0.0f, 0.0f});
        REQUIRE(w.RunUntil([&] { return w.Ready(); }));
        activator.session->ActivateTimeField(msg::TimeFieldKind::Sandevistan, 0.25f, 10 * kSecond);
        w.Step(kFramesPerSecond);

        auto& late = w.AddClient("Late", sim::Script::Idle, Vec3{12.0f, 0.0f, 0.0f});
        REQUIRE(w.RunUntil([&] { return late.session->State() == ClientState::Joined && SameGroup(w, activator, late); }));
        w.Step(kFramesPerSecond / 2);
        CHECK(std::abs(late.sim->Rates().worldRate - 0.25f) < 1e-3f);
    }
    {
        World w([](HostConfig& aConfig) { aConfig.globalTimeFields = true; });
        auto& activator = w.AddClient("Activator", sim::Script::Idle, Vec3{10.0f, 0.0f, 0.0f});
        auto& far = w.AddClient("Far", sim::Script::Idle, Vec3{8000.0f, 0.0f, 0.0f});
        REQUIRE(w.RunUntil([&] { return w.Ready() && SameGroup(w, activator, far); }));
        activator.session->ActivateTimeField(msg::TimeFieldKind::Sandevistan, 0.25f, 4 * kSecond);
        w.Step(kFramesPerSecond);
        CHECK(std::abs(far.sim->Rates().worldRate - 0.25f) < 1e-3f);
    }
}

TEST_CASE("time fields: the host refuses bad activations and ends them on the sender")
{
    World w;
    auto& player = w.AddClient("Player", sim::Script::Idle, Vec3{10.0f, 0.0f, 0.0f});
    REQUIRE(w.RunUntil([&] { return w.Ready() && SameGroup(w, player, w.HostPlayer()); }));
    const TimeUs now = player.session->SessionNow();

    auto send = [&](msg::TimeFieldActivate aActivate)
    {
        const auto bytes = Encode(aActivate);
        player.transport->Send(player.transport->FirstConnection(), Lane::Control, bytes, true);
    };
    msg::TimeFieldActivate base;
    base.activationId = (static_cast<uint32_t>(player.Peer()) << 24) | 0x500u;
    base.start = now;
    base.end = now + 4 * kSecond;
    base.scaleUnits = 2500;

    auto forged = base;
    forged.activationId = (static_cast<uint32_t>(w.HostPlayer().Peer()) << 24) | 0x501u; // someone else's range
    send(forged);
    auto tooStrong = base;
    tooStrong.activationId += 1;
    tooStrong.scaleUnits = 10; // 0.001
    send(tooStrong);
    auto tooLong = base;
    tooLong.activationId += 2;
    tooLong.end = now + 60 * kSecond;
    send(tooLong);
    auto future = base;
    future.activationId += 3;
    future.start = now + 10 * kSecond;
    future.end = future.start + kSecond;
    send(future);
    w.Step(10);

    CHECK_EQ(w.host->Stats().timeFieldsRefused, 4u);
    CHECK(w.host->TimeActivations().empty());
    CHECK(std::abs(w.HostPlayer().sim->Rates().worldRate - 1.0f) < 1e-6f);
}
