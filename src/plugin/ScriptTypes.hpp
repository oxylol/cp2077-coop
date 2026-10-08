#pragma once

#include <RED4ext/RED4ext.hpp>
#include <RED4ext/Scripting/Natives/Vector4.hpp>
#include <RedLib.hpp>

// Plain structs shared with redscript (declared there as `native struct`, see scripts/Cp2077Coop/CoopNative.reds).
namespace Coop
{
// What the bridge reports about the local V.
struct CoopLocalSample
{
    bool valid = false;
    Red::Vector4 position{};
    float yaw = 0.0f;   // degrees, game convention
    float pitch = 0.0f; // degrees
    int32_t locomotion = 0;
    int32_t flags = 0;  // bit 7: female body
};

// Where and how a remote player's puppet should be shown this frame.
struct CoopPuppetPose
{
    Red::Vector4 position{};
    Red::Vector4 velocity{};
    float yaw = 0.0f;
    float pitch = 0.0f;
    float speed = 0.0f; // horizontal m/s
    int32_t locomotion = 0;
    int32_t flags = 0;
    float rate = 1.0f;
};
} // namespace Coop

RTTI_DEFINE_CLASS(Coop::CoopLocalSample, {
    RTTI_PROPERTY(valid);
    RTTI_PROPERTY(position);
    RTTI_PROPERTY(yaw);
    RTTI_PROPERTY(pitch);
    RTTI_PROPERTY(locomotion);
    RTTI_PROPERTY(flags);
});

RTTI_DEFINE_CLASS(Coop::CoopPuppetPose, {
    RTTI_PROPERTY(position);
    RTTI_PROPERTY(velocity);
    RTTI_PROPERTY(yaw);
    RTTI_PROPERTY(pitch);
    RTTI_PROPERTY(speed);
    RTTI_PROPERTY(locomotion);
    RTTI_PROPERTY(flags);
    RTTI_PROPERTY(rate);
});
