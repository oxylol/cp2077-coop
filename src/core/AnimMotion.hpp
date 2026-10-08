#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "core/AnimInput.hpp"
#include "core/Math.hpp"

// Locomotion values for a body that is placed every frame instead of walking (direct drive,
// docs/01-architecture.md §4).
//
// A placed body has no movement of its own, so whatever its animation graph would normally read from the
// character's movement (how fast, which way relative to where it faces, up or down, turning) is worked out here
// from the pose stream the session already sends (position, velocity, yaw) and applied as graph inputs. Which
// graph inputs those are is configured by name (coop.ini [anim] speedInput=... and so on), because the names
// differ per animation setup and are found in game (docs/07-testing-guide.md).
namespace coop
{
// Vertical speed above which a placed player body counts as in the air (jumping or falling), m/s.
constexpr float kInAirSpeed = 2.0f;

struct MotionValues
{
    float speed = 0.0f;     // horizontal, m/s
    float direction = 0.0f; // movement relative to facing, degrees in (-180, 180]: 0 forward, 90 left, -90 right, 180 back
    float vertical = 0.0f;  // m/s, up positive
    float turnRate = 0.0f;  // degrees per second, turning left positive
    bool moving = false;
    float yaw = 0.0f;          // facing, degrees (HeadingDegrees convention)
    float heading = 0.0f;      // movement heading, degrees; kept while standing
    float acceleration = 0.0f; // horizontal speed change, m/s per second (smoothed)

    bool operator==(const MotionValues&) const = default;
};

// The graph inputs the values go to (CName hashes); 0 = not sent. moving is a bool input, the rest are floats.
struct MotionInputNames
{
    uint64_t speed = 0;
    uint64_t direction = 0;
    uint64_t vertical = 0;
    uint64_t turnRate = 0;
    uint64_t moving = 0;

    [[nodiscard]] bool Any() const { return speed != 0 || direction != 0 || vertical != 0 || turnRate != 0 || moving != 0; }
};

// Heading of a horizontal direction in the game's yaw convention: degrees in [0, 360), 0 facing +Y, 90 facing -X
// (counterclockwise seen from above), the same as Vector4.ToRotation(forward).Yaw. [VERIFY] sign in game.
inline float HeadingDegrees(const Vec3& aDirection)
{
    return WrapDegrees(std::atan2(-aDirection.x, aDirection.y) * 180.0f / 3.14159265358979f);
}

// Turns a stream of (velocity, yaw) samples into MotionValues. One per body.
class MotionTracker
{
public:
    static constexpr float kMovingSpeed = 0.2f;     // m/s; slower counts as standing
    static constexpr float kTurnSmoothing = 0.15f;  // seconds; yaw arrives in steps, so the rate is smoothed
    static constexpr float kAccelSmoothing = 0.2f;  // seconds

    // aVelocity in m/s, aYaw in degrees, aDt in seconds since the previous call (0 on the first).
    MotionValues Update(const Vec3& aVelocity, float aYaw, float aDt)
    {
        const float previousSpeed = m_values.speed;
        m_values.speed = aVelocity.Length2D();
        m_values.vertical = aVelocity.z;
        m_values.moving = m_values.speed > kMovingSpeed;
        m_values.yaw = WrapDegrees(aYaw);
        // Standing still keeps the last direction, so a stop blends out the way the body was going.
        if (m_values.moving)
        {
            m_values.heading = HeadingDegrees(aVelocity);
            m_values.direction = DeltaDegrees(aYaw, m_values.heading);
        }
        else if (!m_started)
        {
            m_values.heading = m_values.yaw;
        }

        if (m_started && aDt > 0.0f)
        {
            const float raw = DeltaDegrees(m_yaw, aYaw) / aDt;
            const float blend = aDt / (kTurnSmoothing + aDt);
            m_values.turnRate += (raw - m_values.turnRate) * blend;
            const float accel = (m_values.speed - previousSpeed) / aDt;
            const float accelBlend = aDt / (kAccelSmoothing + aDt);
            m_values.acceleration += (accel - m_values.acceleration) * accelBlend;
        }
        m_yaw = aYaw;
        m_started = true;
        return m_values;
    }

    void Reset() { *this = MotionTracker{}; }
    [[nodiscard]] const MotionValues& Last() const { return m_values; }

private:
    bool m_started = false;
    float m_yaw = 0.0f;
    MotionValues m_values;
};

// Velocity of something that is only seen as positions (the dev panel's mirror body, placed next to V every frame).
class PositionVelocity
{
public:
    static constexpr float kSmoothing = 0.1f; // seconds
    static constexpr float kJump = 20.0f;     // a step longer than this is a teleport, not movement

    Vec3 Update(const Vec3& aPosition, float aDt)
    {
        if (!m_started || aDt <= 0.0f || Distance(aPosition, m_position) > kJump)
        {
            m_velocity = {};
        }
        else
        {
            const Vec3 raw = (aPosition - m_position) * (1.0f / aDt);
            const float blend = aDt / (kSmoothing + aDt);
            m_velocity = m_velocity + (raw - m_velocity) * blend;
        }
        m_position = aPosition;
        m_started = true;
        return m_velocity;
    }

private:
    bool m_started = false;
    Vec3 m_position;
    Vec3 m_velocity;
};

// Unit horizontal vector for a heading (the inverse of HeadingDegrees).
inline Vec3 HeadingVector(float aDegrees)
{
    const float radians = aDegrees * 3.14159265358979f / 180.0f;
    return {-std::sin(radians), std::cos(radians), 0.0f};
}

// The engine's name hash (CName: FNV-1a 64), for names this code builds itself.
constexpr uint64_t NameHashOf(const char* aText)
{
    uint64_t hash = 0xCBF29CE484222325ull;
    for (; aText && *aText; ++aText)
    {
        hash ^= static_cast<uint64_t>(static_cast<uint8_t>(*aText));
        hash *= 0x100000001B3ull;
    }
    return hash;
}

// The walking input of V's own animation graph: the feature "playerLocomotion" (animAnimFeature_PlayerMovement,
// round I graph dump). The player's movement sets it natively every frame, so it is never captured; a placed
// player body (TPP_Player) gets it built from its motion values instead (round K: crouch animates, running
// doesn't). [VERIFY] round L: which fields the graph reads, and whether the directions are world or local.
constexpr uint64_t kPlayerMovementInput = NameHashOf("playerLocomotion");
constexpr uint64_t kPlayerMovementClass = NameHashOf("animAnimFeature_PlayerMovement");

inline AnimInput MotionToPlayerMovement(const MotionValues& aValues)
{
    AnimInput input;
    input.kind = AnimInputKind::Feature;
    input.name = kPlayerMovementInput;
    input.featureClass = kPlayerMovementClass;
    auto add = [&](const char* aName, const AnimValue& aValue) { input.props.push_back({NameHashOf(aName), aValue}); };
    const Vec3 move = HeadingVector(aValues.heading);
    const Vec3 facing = HeadingVector(aValues.yaw);
    // animAnimFeature_Movement
    add("movementDirection", AnimValue::FromVector(move.x, move.y, 0.0f, 0.0f));
    add("speed", AnimValue::FromFloat(aValues.speed));
    add("desiredSpeed", AnimValue::FromFloat(aValues.speed));
    add("stabilizedSpeed", AnimValue::FromFloat(aValues.speed));
    add("acceleration", AnimValue::FromFloat(aValues.acceleration));
    add("strafeYaw", AnimValue::FromFloat(aValues.direction));
    add("yawSpeed", AnimValue::FromFloat(aValues.turnRate));
    // animAnimFeature_PlayerMovement
    add("facingDirection", AnimValue::FromVector(facing.x, facing.y, 0.0f, 0.0f));
    add("verticalSpeed", AnimValue::FromFloat(aValues.vertical));
    add("movementDirectionHorizontalAngle", AnimValue::FromFloat(aValues.direction));
    add("inAir", AnimValue::FromBool(std::fabs(aValues.vertical) > kInAirSpeed));
    return input;
}

// The player graph's switch between first- and third-person upper body: the feature "TPPRepresentation"
// (gameAnimFeature_TPPRepresentation { IsActive }, in V's root, shadow and deformations graphs). A spawned player
// body runs its torso and arms as if first person (round L: legs right, torso broken). [VERIFY] round M.
constexpr uint64_t kTppRepresentationInput = NameHashOf("TPPRepresentation");
constexpr uint64_t kTppRepresentationClass = NameHashOf("gameAnimFeature_TPPRepresentation");

inline AnimInput TppRepresentationInput(bool aActive)
{
    AnimInput input;
    input.kind = AnimInputKind::Feature;
    input.name = kTppRepresentationInput;
    input.featureClass = kTppRepresentationClass;
    input.props.push_back({NameHashOf("IsActive"), AnimValue::FromBool(aActive)});
    return input;
}

// The configured graph inputs with this frame's values.
inline std::vector<AnimInput> MotionToInputs(const MotionValues& aValues, const MotionInputNames& aNames)
{
    std::vector<AnimInput> inputs;
    auto addFloat = [&](uint64_t aName, float aValue)
    {
        if (aName == 0)
            return;
        AnimInput input;
        input.kind = AnimInputKind::Float;
        input.name = aName;
        input.value = AnimValue::FromFloat(aValue);
        inputs.push_back(input);
    };
    addFloat(aNames.speed, aValues.speed);
    addFloat(aNames.direction, aValues.direction);
    addFloat(aNames.vertical, aValues.vertical);
    addFloat(aNames.turnRate, aValues.turnRate);
    if (aNames.moving != 0)
    {
        AnimInput input;
        input.kind = AnimInputKind::Bool;
        input.name = aNames.moving;
        input.value = AnimValue::FromBool(aValues.moving);
        inputs.push_back(input);
    }
    return inputs;
}
} // namespace coop
