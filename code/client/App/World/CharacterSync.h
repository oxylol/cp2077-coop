#pragma once

#include "Red/TypeInfo/Macros/Definition.hpp"

struct EntityComponent;
struct RemoteStateComponent;

// What the others see of a player besides where they are: crouching, the weapon in hand, aiming, reloading and
// shooting. Reads this player's state machine and sends what changed (client::CharacterStateRequest, shots in
// client::CharacterShotRequest); shows the others' on their characters. The game side is CharacterSync.reds.
struct CharacterSync : RED4ext::IScriptable
{
    RTTI_IMPL_TYPEINFO(CharacterSync)
    RTTI_IMPL_ALLOCATOR();

    void OnInitialize();
    void OnConnected();
    void OnDisconnected();

    // From CharacterSync.reds: this player's state (ReadLocalState), and each shot they fire.
    void SetLocalState(int32_t aLocomotion, int32_t aUpperBody, int32_t aWeaponState, Red::TweakDBID aWeapon);
    void OnLocalShot();

protected:
    void HandleCharacterState(const PacketEvent<server::NotifyCharacterState>& aMessage);
    void HandleCharacterShot(const PacketEvent<server::NotifyCharacterShot>& aMessage);

    void SendLocal();
    void Show(flecs::entity aEntity, const EntityComponent& acEntity, RemoteStateComponent& aState);

private:
    struct State
    {
        uint32_t Locomotion;
        uint32_t UpperBody;
        uint32_t WeaponState;
        uint64_t Weapon;

        bool operator==(const State&) const = default;
    };

    std::optional<State> m_local; // read this run (SetLocalState)
    std::optional<State> m_sent;
    uint64_t m_sentId{0}; // the character m_sent was for
    uint32_t m_shots{0};
    flecs::system m_sender;
    flecs::system m_shower;
};

RTTI_DEFINE_CLASS(CharacterSync, {
    RTTI_ALIAS("CyberpunkMP.World.CharacterSync");
    RTTI_METHOD(SetLocalState);
    RTTI_METHOD(OnLocalShot);
});
