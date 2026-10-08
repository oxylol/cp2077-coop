// Animation inputs (core/AnimInput.hpp): the message codec, the session path from one player's game to the
// others' (deterministic in-memory network, tests/SimWorld.hpp), and the motion inputs for placed bodies
// (core/AnimMotion.hpp).

#include "SimWorld.hpp"
#include "Test.hpp"
#include "core/AnimMotion.hpp"
#include "protocol/Codec.hpp"

using namespace coop;

namespace
{
AnimInput Feature(uint64_t aName, uint64_t aClass, std::vector<AnimProp> aProps)
{
    AnimInput input;
    input.kind = AnimInputKind::Feature;
    input.name = aName;
    input.featureClass = aClass;
    input.props = std::move(aProps);
    return input;
}

AnimInput Simple(AnimInputKind aKind, uint64_t aName, AnimValue aValue = {})
{
    AnimInput input;
    input.kind = aKind;
    input.name = aName;
    input.value = aValue;
    return input;
}

// What SessionRunner::Pump does each frame in the game: hand the game's captured inputs to the session.
void PublishEveryFrame(test::World& aWorld)
{
    aWorld.onFrame = [](test::Machine& aMachine)
    {
        std::vector<AnimInput> inputs;
        aMachine.sim->CaptureAnimInputs(inputs);
        if (!inputs.empty())
            aMachine.session->PublishAnimInputs(inputs);
    };
}
} // namespace

TEST_CASE("anim: every kind of input survives the codec")
{
    msg::PlayerAnim message;
    message.peer = 2;
    message.sessionTimeUs = 123'456'789;
    message.full = true;
    message.inputs.push_back(Feature(0x1111, 0x2222,
                                     {{0xA, AnimValue::FromFloat(1.5f)},
                                      {0xB, AnimValue::FromInt(-7)},
                                      {0xC, AnimValue::FromBool(true)},
                                      {0xD, AnimValue::FromName(0xFEEDFACECAFEBEEFull)},
                                      {0xE, AnimValue::FromVector(1.0f, -2.0f, 3.5f, 0.0f)}}));
    message.inputs.push_back(Simple(AnimInputKind::Float, 0x3333, AnimValue::FromFloat(-0.25f)));
    message.inputs.push_back(Simple(AnimInputKind::Int, 0x4444, AnimValue::FromInt(42)));
    message.inputs.push_back(Simple(AnimInputKind::Bool, 0x5555, AnimValue::FromBool(true)));
    message.inputs.push_back(Simple(AnimInputKind::Vector, 0x6666, AnimValue::FromVector(0.1f, 0.2f, 0.3f, 1.0f)));
    message.inputs.push_back(Simple(AnimInputKind::Event, 0x7777));

    const auto bytes = Encode(message);
    REQUIRE(!bytes.empty());
    msg::PlayerAnim decoded;
    REQUIRE(Decode(bytes, decoded));
    CHECK_EQ(decoded.peer, message.peer);
    CHECK_EQ(decoded.sessionTimeUs, message.sessionTimeUs);
    CHECK(decoded.full);
    REQUIRE(decoded.inputs.size() == message.inputs.size());
    for (size_t i = 0; i < message.inputs.size(); ++i)
        CHECK(decoded.inputs[i] == message.inputs[i]);

    // A feature with every value type costs bytes, not kilobytes.
    CHECK(bytes.size() < 200);
}

TEST_CASE("anim: malformed inputs are rejected")
{
    // A message whose single input has an out-of-range kind.
    WriteStream stream;
    PeerId peer = 1;
    int64_t time = 0;
    bool full = false;
    int32_t count = 1;
    uint8_t badKind = static_cast<uint8_t>(AnimInputKind::Count);
    uint64_t name = 5;
    stream.U8(peer);
    stream.I64(time);
    stream.Bool(full);
    stream.IntRange(count, 0, kMaxAnimInputsPerMessage);
    stream.U8(badKind);
    stream.U64(name);
    std::vector<uint8_t> forged;
    WriteHeader(forged, MsgId::PlayerAnim, 0);
    const auto payload = stream.Finish();
    forged.insert(forged.end(), payload.begin(), payload.end());

    msg::PlayerAnim decoded;
    CHECK(!Decode(forged, decoded));
}

TEST_CASE("anim: one player's inputs reach the others, events exactly once, changes only")
{
    test::World world;
    auto& dancer = world.AddClient("Dancer", sim::Script::Circle, Vec3{10.0f, 0.0f, 0.0f});
    auto& watcher = world.AddClient("Watcher", sim::Script::Idle, Vec3{-10.0f, 0.0f, 0.0f});
    dancer.sim->SetAnimOutput(true);
    PublishEveryFrame(world);

    REQUIRE(world.RunUntil([&] { return world.Ready(); }));
    const PeerId dancerPeer = dancer.Peer();

    // Run five seconds.
    world.Step(300);

    for (auto* machine : {&watcher, &world.HostPlayer()})
    {
        const auto& known = machine->sim->KnownAnim();
        REQUIRE(known.count(dancerPeer) == 1);
        const auto& remote = known.at(dancerPeer);

        // The latest values are there, unchanged.
        AnimInput feature;
        feature.kind = AnimInputKind::Feature;
        feature.name = sim::kSimAnimLocomotion;
        REQUIRE(remote.inputs.count(feature.Key()) == 1);
        const auto& got = remote.inputs.at(feature.Key());
        CHECK_EQ(got.featureClass, sim::kSimAnimFeatureClass);
        REQUIRE(got.props.size() == 2u);
        CHECK_NEAR(got.props[0].value.f[0], dancer.sim->Config().speed, 1e-6f);
        CHECK_EQ(got.props[1].value.i, 1);

        AnimInput speed;
        speed.kind = AnimInputKind::Float;
        speed.name = sim::kSimAnimSpeed;
        REQUIRE(remote.inputs.count(speed.Key()) == 1);

        // Every event once (the last one may still be in flight).
        CHECK(remote.events + 1 >= dancer.sim->AnimEventsProduced());
        CHECK(remote.events <= dancer.sim->AnimEventsProduced());

        // Full sets every 2 s; otherwise only changes and events: the values never change here, so about one message
        // per event plus the full sets, far below the 15 Hz ceiling.
        CHECK(remote.fullSets >= 2u);
        CHECK(remote.messages <= remote.events + remote.fullSets + 2);
    }
    CHECK(world.host->Stats().animRelayed > 0);
}

TEST_CASE("anim: a late joiner gets the full set within two seconds")
{
    test::World world;
    auto& dancer = world.AddClient("Dancer", sim::Script::Idle, Vec3{10.0f, 0.0f, 0.0f});
    dancer.sim->SetAnimOutput(true);
    PublishEveryFrame(world);
    REQUIRE(world.RunUntil([&] { return world.Ready(); }));
    world.Step(180);

    auto& late = world.AddClient("Late", sim::Script::Idle, Vec3{-10.0f, 0.0f, 0.0f});
    REQUIRE(world.RunUntil([&] { return world.Ready(); }));
    const PeerId dancerPeer = dancer.Peer();
    REQUIRE(world.RunUntil(
        [&]
        {
            const auto& known = late.sim->KnownAnim();
            return known.count(dancerPeer) == 1 && known.at(dancerPeer).inputs.size() == 2;
        },
        150)); // 2.5 s
}

// --- motion inputs for placed bodies (core/AnimMotion.hpp) ---------------------------------------------------------

TEST_CASE("anim motion: direction is relative to facing, in the game's yaw convention")
{
    // Facing +Y (yaw 0): forward, left (-X), right (+X), back.
    MotionTracker tracker;
    auto values = tracker.Update({0.0f, 3.0f, 0.0f}, 0.0f, 0.0f);
    CHECK(values.moving);
    CHECK_NEAR(values.speed, 3.0f, 1e-4f);
    CHECK_NEAR(values.direction, 0.0f, 1e-3f);
    CHECK_NEAR(tracker.Update({-2.0f, 0.0f, 0.0f}, 0.0f, 0.016f).direction, 90.0f, 1e-3f);
    CHECK_NEAR(tracker.Update({2.0f, 0.0f, 0.0f}, 0.0f, 0.016f).direction, -90.0f, 1e-3f);
    CHECK_NEAR(std::abs(tracker.Update({0.0f, -2.0f, 0.0f}, 0.0f, 0.016f).direction), 180.0f, 1e-3f);

    // Facing -X (yaw 90) and moving -X is forward; facing +X is yaw 270 or -90, and moving -Y then goes right.
    MotionTracker turned;
    CHECK_NEAR(turned.Update({-4.0f, 0.0f, 0.0f}, 90.0f, 0.0f).direction, 0.0f, 1e-3f);
    CHECK_NEAR(turned.Update({4.0f, 0.0f, 0.0f}, 270.0f, 0.016f).direction, 0.0f, 1e-3f);
    CHECK_NEAR(turned.Update({4.0f, 0.0f, 0.0f}, -90.0f, 0.016f).direction, 0.0f, 1e-3f);
    CHECK_NEAR(turned.Update({0.0f, -4.0f, 0.0f}, -90.0f, 0.016f).direction, -90.0f, 1e-3f);

    // Vertical speed passes through; horizontal speed ignores it.
    const auto falling = turned.Update({0.0f, 0.0f, -6.0f}, -90.0f, 0.016f);
    CHECK_NEAR(falling.vertical, -6.0f, 1e-4f);
    CHECK_NEAR(falling.speed, 0.0f, 1e-4f);
}

TEST_CASE("anim motion: stopping keeps the last direction, slow drift is standing")
{
    MotionTracker tracker;
    tracker.Update({2.0f, 0.0f, 0.0f}, 0.0f, 0.0f); // moving right
    const auto stopped = tracker.Update({0.05f, 0.1f, 0.0f}, 0.0f, 0.016f);
    CHECK(!stopped.moving);
    CHECK_NEAR(stopped.direction, -90.0f, 1e-3f);
}

TEST_CASE("anim motion: turn rate is smoothed and wraps through 0/360")
{
    MotionTracker tracker;
    const float dt = 1.0f / 60.0f;
    float yaw = 350.0f;
    tracker.Update({}, yaw, 0.0f);
    // Turn left at 90 deg/s for one second, crossing 360 -> 0.
    MotionValues values;
    for (int i = 0; i < 60; ++i)
    {
        yaw = WrapDegrees(yaw + 90.0f * dt);
        values = tracker.Update({}, yaw, dt);
    }
    CHECK_NEAR(values.turnRate, 90.0f, 1.0f);
    // Turning right gives a negative rate.
    for (int i = 0; i < 60; ++i)
    {
        yaw = WrapDegrees(yaw - 45.0f * dt);
        values = tracker.Update({}, yaw, dt);
    }
    CHECK_NEAR(values.turnRate, -45.0f, 1.0f);
    // A single step does not jump straight to the raw rate.
    MotionTracker fresh;
    fresh.Update({}, 0.0f, 0.0f);
    CHECK(fresh.Update({}, 10.0f, dt).turnRate < 300.0f);
}

TEST_CASE("anim motion: velocity from positions, teleports reset it")
{
    PositionVelocity estimator;
    const float dt = 1.0f / 60.0f;
    Vec3 position{100.0f, 200.0f, 10.0f};
    Vec3 velocity;
    for (int i = 0; i < 90; ++i)
    {
        position = position + Vec3{0.0f, 3.0f, 0.0f} * dt;
        velocity = estimator.Update(position, dt);
    }
    CHECK_NEAR(velocity.y, 3.0f, 0.05f);
    CHECK_NEAR(velocity.x, 0.0f, 0.01f);
    // A jump of 500 m (fast travel) is not 30 km/s.
    position = position + Vec3{500.0f, 0.0f, 0.0f};
    velocity = estimator.Update(position, dt);
    CHECK_NEAR(velocity.Length(), 0.0f, 1e-4f);
}

TEST_CASE("anim motion: only the configured inputs are produced")
{
    MotionValues values;
    values.speed = 4.5f;
    values.direction = -30.0f;
    values.moving = true;
    MotionInputNames names;
    CHECK(!names.Any());
    CHECK(MotionToInputs(values, names).empty());

    names.speed = 0x1001;
    names.moving = 0x1002;
    const auto inputs = MotionToInputs(values, names);
    REQUIRE(inputs.size() == 2u);
    CHECK(inputs[0].kind == AnimInputKind::Float);
    CHECK_EQ(inputs[0].name, 0x1001u);
    CHECK_NEAR(inputs[0].value.f[0], 4.5f, 1e-6f);
    CHECK(inputs[1].kind == AnimInputKind::Bool);
    CHECK_EQ(inputs[1].value.i, 1);

    // They travel like any other input.
    msg::PlayerAnim message;
    message.inputs = inputs;
    msg::PlayerAnim decoded;
    REQUIRE(Decode(Encode(message), decoded));
    CHECK(decoded.inputs == inputs);
}

TEST_CASE("anim motion: the walking feature for V's graph (playerLocomotion)")
{
    // The engine's CName hash (FNV-1a 64), checked against values computed independently.
    CHECK_EQ(NameHashOf("playerLocomotion"), 0x23f2d4b0d459d639ull);
    CHECK_EQ(NameHashOf("animAnimFeature_PlayerMovement"), 0xc6c57a72dc0dae36ull);
    CHECK_EQ(NameHashOf("speed"), 0x2281498aa0200e40ull);

    // Facing +Y (yaw 0) and strafing right (moving towards +X, heading 270): direction -90.
    MotionTracker tracker;
    tracker.Update({3.0f, 0.0f, 0.0f}, 0.0f, 0.0f);
    const auto values = tracker.Update({3.0f, 0.0f, 0.0f}, 0.0f, 0.1f);
    CHECK_NEAR(values.heading, 270.0f, 0.01f);
    CHECK_NEAR(values.direction, -90.0f, 0.01f);

    const auto feature = MotionToPlayerMovement(values);
    CHECK(feature.kind == AnimInputKind::Feature);
    CHECK_EQ(feature.name, kPlayerMovementInput);
    CHECK_EQ(feature.featureClass, kPlayerMovementClass);
    auto prop = [&](const char* aName) -> const AnimValue* {
        for (const auto& p : feature.props)
            if (p.name == NameHashOf(aName))
                return &p.value;
        return nullptr;
    };
    REQUIRE(prop("speed") != nullptr);
    CHECK_NEAR(prop("speed")->f[0], 3.0f, 1e-5f);
    REQUIRE(prop("movementDirection") != nullptr);
    CHECK_NEAR(prop("movementDirection")->f[0], 1.0f, 1e-5f); // towards +X
    CHECK_NEAR(prop("movementDirection")->f[1], 0.0f, 1e-5f);
    REQUIRE(prop("facingDirection") != nullptr);
    CHECK_NEAR(prop("facingDirection")->f[0], 0.0f, 1e-5f); // facing +Y
    CHECK_NEAR(prop("facingDirection")->f[1], 1.0f, 1e-5f);
    REQUIRE(prop("movementDirectionHorizontalAngle") != nullptr);
    CHECK_NEAR(prop("movementDirectionHorizontalAngle")->f[0], -90.0f, 0.01f);
    REQUIRE(prop("inAir") != nullptr);
    CHECK_EQ(prop("inAir")->i, 0);

    // Standing still keeps the heading, so the body stops facing the way it went; falling counts as in the air.
    const auto stopped = tracker.Update({0.0f, 0.0f, -6.0f}, 0.0f, 0.1f);
    CHECK_NEAR(stopped.heading, 270.0f, 0.01f);
    CHECK(stopped.acceleration < 0.0f);
    const auto falling = MotionToPlayerMovement(stopped);
    for (const auto& p : falling.props)
        if (p.name == NameHashOf("inAir"))
            CHECK_EQ(p.value.i, 1);

    // It travels like any other input.
    msg::PlayerAnim message;
    message.inputs = {feature};
    msg::PlayerAnim decoded;
    REQUIRE(Decode(Encode(message), decoded));
    CHECK(decoded.inputs == message.inputs);
}

TEST_CASE("anim motion: the third-person switch for V's graph (TPPRepresentation)")
{
    CHECK_EQ(kTppRepresentationInput, 0x9b3a41087f39d0ceull);
    CHECK_EQ(kTppRepresentationClass, 0x07d507d2f7ebfbe6ull);
    const auto input = TppRepresentationInput(true);
    CHECK(input.kind == AnimInputKind::Feature);
    REQUIRE(input.props.size() == 1u);
    CHECK_EQ(input.props[0].name, 0x30bc6a267026df9full); // IsActive
    CHECK(input.props[0].value.type == AnimValueType::Bool);
    CHECK_EQ(input.props[0].value.i, 1);
    CHECK_EQ(TppRepresentationInput(false).props[0].value.i, 0);
}
