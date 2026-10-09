#include "CharacterStateComponent.h"

server::NotifyCharacterState CharacterStateComponent::ToMessage(flecs::entity aEntity) const
{
    server::NotifyCharacterState message;
    message.set_id(aEntity);
    message.set_locomotion(Locomotion);
    message.set_upper_body(UpperBody);
    message.set_weapon_state(WeaponState);
    message.set_weapon(Weapon);
    return message;
}
