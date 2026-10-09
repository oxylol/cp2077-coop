#pragma once

// Another player's stance and weapon (server::NotifyCharacterState), and how far their character here shows it
// (CharacterSync).
struct RemoteStateComponent
{
    uint32_t Locomotion{0};
    uint32_t UpperBody{0};
    uint32_t WeaponState{0};
    uint64_t Weapon{0};
    float AimPitch{0.f}; // from its moves (server::NotifyEntityMove)

    // What the character shows: nothing yet until Shown.
    bool Shown{false};
    uint32_t ShownLocomotion{0};
    uint32_t ShownUpperBody{0};
    uint32_t ShownWeaponState{0};
    bool ShownArmed{false};
    bool ShownReloading{false};
    bool InCombatMode{false}; // the animation system's, which a character starts out of
    // The weapon in the character's hand: until it's the one above, CharacterSync tries again (WeaponAttempts).
    bool WeaponInHand{false};
    int32_t WeaponAttempts{0};
    // Runs since the stance was last applied: it's applied again now and then, in case the game reset it.
    uint32_t SinceRefresh{0};
};
