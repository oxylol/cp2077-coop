#pragma once

// A character's stance and weapon, as its player last sent them (client.CharacterStateRequest): sent on to the
// others when it changes, and to whoever loads the character later.
struct CharacterStateComponent
{
    uint32_t Locomotion{0};
    uint32_t UpperBody{0};
    uint32_t WeaponState{0};
    uint64_t Weapon{0};

    server::NotifyCharacterState ToMessage(flecs::entity aEntity) const;
};
