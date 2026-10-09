#pragma once

struct MovementComponent
{
    glm::vec3 Position;
    glm::vec3 Rotation;
    float Velocity;
    uint64_t Tick;
    float AimPitch{0.f}; // a character's (client::MoveEntityRequest)

    static void Register(flecs::world& aWorld);
};