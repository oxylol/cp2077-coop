#include <algorithm>
#include <cmath>

#include "Test.hpp"
#include "core/TimeField.hpp"

using namespace coop;

namespace
{
TimeActivation MakeActivation(uint32_t aId, PeerId aPeer, float aScale, double aStartS, double aEndS)
{
    TimeActivation activation;
    activation.id = aId;
    activation.peer = aPeer;
    activation.scale = aScale;
    activation.start = FromSeconds(aStartS);
    activation.end = FromSeconds(aEndS);
    activation.easeIn = 300 * kUsPerMs;
    activation.easeOut = 300 * kUsPerMs;
    return activation;
}
} // namespace

TEST_CASE("timefield: curve shape")
{
    const auto a = MakeActivation(1, 1, 0.25f, 1.0, 9.0);
    CHECK_NEAR(ActivationCurve(a, FromSeconds(0.5)), 1.0, 1e-6);
    CHECK_NEAR(ActivationCurve(a, FromSeconds(1.15)), 0.625, 1e-3); // smoothstep midpoint
    CHECK_NEAR(ActivationCurve(a, FromSeconds(5.0)), 0.25, 1e-6);
    CHECK_NEAR(ActivationCurve(a, FromSeconds(9.15)), 0.625, 1e-3);
    CHECK_NEAR(ActivationCurve(a, FromSeconds(9.5)), 1.0, 1e-6);
}

TEST_CASE("timefield: world time for one 8 s activation at 0.25")
{
    FieldSchedule schedule;
    schedule.SetAnchor(0, 0.0);
    schedule.Upsert(MakeActivation(1, 1, 0.25f, 1.0, 9.0));

    // 1 s normal + 0.3*(1+0.25)/2 ease-in + 7.7*0.25 hold + 0.3*(1+0.25)/2 ease-out + 0.7 normal = 4.0 s
    CHECK_NEAR(schedule.WorldTimeAt(FromSeconds(10.0)), 4'000'000.0, 1.0);
    CHECK_NEAR(schedule.WorldTimeAt(FromSeconds(1.0)), 1'000'000.0, 1.0);
    // Clock debt after the effect: 6 s, as described in docs/01-architecture.md §8.6.
    CHECK_NEAR(FromSeconds(10.0) - schedule.WorldTimeAt(FromSeconds(10.0)), 6'000'000.0, 1.0);
}

TEST_CASE("timefield: overlapping activations resolve proportionally")
{
    FieldSchedule schedule;
    schedule.SetAnchor(0, 0.0);
    schedule.Upsert(MakeActivation(1, 1, 0.25f, 0.0, 10.0)); // peer 1, strong
    schedule.Upsert(MakeActivation(2, 2, 0.5f, 0.0, 10.0));  // peer 2, weaker

    const TimeUs t = FromSeconds(5.0);
    CHECK_NEAR(schedule.WorldRate(t), 0.25, 1e-6);
    CHECK_NEAR(schedule.PersonalRate(1, t), 1.0, 1e-6);  // strongest runs at real speed
    CHECK_NEAR(schedule.PersonalRate(2, t), 0.5, 1e-6);  // 2x the world, half of peer 1
    CHECK_NEAR(schedule.PersonalRate(3, t), 0.25, 1e-6); // non-activator moves with the world

    // Relative speeds: peer 1 is 4x the world, peer 2 is 2x the world.
    CHECK_NEAR(schedule.PersonalRate(1, t) / schedule.WorldRate(t), 4.0, 1e-5);
    CHECK_NEAR(schedule.PersonalRate(2, t) / schedule.WorldRate(t), 2.0, 1e-5);
}

TEST_CASE("timefield: same activations in any order give the same world time")
{
    const auto a = MakeActivation(10, 1, 0.3f, 0.2, 4.0);
    const auto b = MakeActivation(11, 2, 0.5f, 1.1, 6.5);
    const auto c = MakeActivation(12, 3, 0.2f, 3.0, 3.2); // cancelled during its ease-in

    FieldSchedule first;
    first.SetAnchor(0, 0.0);
    first.Upsert(a);
    first.Upsert(b);
    first.Upsert(c);

    FieldSchedule second;
    second.SetAnchor(0, 0.0);
    second.Upsert(c);
    second.Upsert(a);
    second.Upsert(b);

    for (double s : {0.5, 1.2, 3.1, 3.4, 5.0, 8.0})
        CHECK_EQ(first.WorldTimeAt(FromSeconds(s)), second.WorldTimeAt(FromSeconds(s)));
}

TEST_CASE("timefield: early cancel and collection keep machines identical")
{
    FieldSchedule a;
    a.SetAnchor(0, 0.0);
    a.Upsert(MakeActivation(1, 1, 0.25f, 1.0, 9.0));
    a.SetEnd(1, FromSeconds(4.0)); // the activator cancels early

    FieldSchedule b = a;

    // Machine A collects right after the effect, machine B much later; both must agree afterwards.
    a.Collect(FromSeconds(4.5));
    b.Collect(FromSeconds(30.0));
    CHECK(!a.HasActivations());
    CHECK(!b.HasActivations());
    CHECK_EQ(a.AnchorTime(), b.AnchorTime());
    for (double s : {5.0, 12.0, 60.0})
        CHECK_EQ(a.WorldTimeAt(FromSeconds(s)), b.WorldTimeAt(FromSeconds(s)));

    // 1 + 0.1875 + 2.7*0.25 + 0.1875 = 2.05 s of world time by T = 4.3 s.
    CHECK_NEAR(a.WorldTimeAt(FromSeconds(4.3)), 2'050'000.0, 1.0);
}

TEST_CASE("timefield: clock follower absorbs a late start")
{
    FieldSchedule schedule;
    schedule.SetAnchor(0, 0.0);
    TimeActivation activation = MakeActivation(1, 1, 0.25f, 0.0, 8.0);
    schedule.Upsert(activation);

    ClockFollower follower;
    const TimeUs frame = 16'667;
    const TimeUs messageArrives = 75 * kUsPerMs; // one-way latency before this machine knows
    double engineWorld = 0.0;
    double worstAfterSettle = 0.0;

    for (TimeUs t = 0; t < FromSeconds(3.0); t += frame)
    {
        float dilation = 1.0f;
        if (t >= messageArrives)
            dilation = follower.Compute(schedule.WorldRate(t), schedule.WorldTimeAt(t), engineWorld);
        engineWorld += static_cast<double>(dilation) * static_cast<double>(frame);

        const double error = std::fabs(schedule.WorldTimeAt(t + frame) - engineWorld);
        if (t > FromSeconds(0.6))
            worstAfterSettle = std::max(worstAfterSettle, error);
    }
    CHECK(worstAfterSettle < 1'000.0); // under 1 ms of world time after 0.6 s
}
