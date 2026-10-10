#include "CharacterSync.h"

#include <set>

#include "App/Components/AttachedComponent.h"
#include "App/Components/EntityComponent.h"
#include "App/Components/RemoteStateComponent.h"
#include "App/Network/NetworkService.h"
#include "NetworkWorldSystem.h"

namespace
{
// The others' states are shown 10 times a second, and applied again every this many times (2 s), in case the game
// reset them.
constexpr float kShowInterval = 0.1f;
constexpr uint32_t kRefreshRuns = 20;
// A new weapon is first drawn with the animation; when the character still doesn't hold it after this many runs, it's
// put in its hand directly (and again every as many runs, while it isn't).
constexpr int32_t kWeaponRetryRuns = 15;

// A CharacterSync.reds function that failed (missing, or changed): logged once, not 10 times a second.
void Failed(const char* acFunction)
{
    static std::set<std::string> s_failed;
    if (s_failed.insert(acFunction).second)
        spdlog::warn("[CharacterSync] CharacterSync.reds {} failed", acFunction);
}
} // namespace

void CharacterSync::OnInitialize()
{
    const auto pNetworkService = Core::Container::Get<NetworkService>();
    pNetworkService->RegisterHandler<&CharacterSync::HandleCharacterState>(this);
    pNetworkService->RegisterHandler<&CharacterSync::HandleCharacterShot>(this);
}

void CharacterSync::OnConnected()
{
    const auto pWorld = Red::GetGameSystem<NetworkWorldSystem>();
    const auto pNetworkService = Core::Container::Get<NetworkService>();

    m_sent.reset();
    m_shots = 0;

    m_sender = pWorld->system("Send character state")
        .kind(flecs::OnUpdate)
        .interval(1.f / pNetworkService->GetServerSettings().get_update_rate())
        .run([this](flecs::iter&) { SendLocal(); });

    m_shower = pWorld->system<const EntityComponent, RemoteStateComponent>("Show character states")
        .interval(kShowInterval)
        .each([this](flecs::entity aEntity, const EntityComponent& acEntity, RemoteStateComponent& aState)
        {
            Show(aEntity, acEntity, aState);
        });
}

void CharacterSync::OnDisconnected()
{
    if (m_sender)
        m_sender.destruct();
    if (m_shower)
        m_shower.destruct();

    m_local.reset();
    m_sent.reset();
    m_shots = 0;
    m_aimPitch = 0.f;
}

void CharacterSync::SetLocalState(int32_t aLocomotion, int32_t aUpperBody, int32_t aWeaponState, Red::TweakDBID aWeapon,
                                  float aAimPitch)
{
    m_local = State{static_cast<uint32_t>(aLocomotion), static_cast<uint32_t>(aUpperBody),
                    static_cast<uint32_t>(aWeaponState), aWeapon.value};
    m_aimPitch = aAimPitch;
}

void CharacterSync::OnLocalShot()
{
    const auto pNetworkService = Core::Container::Get<NetworkService>();
    if (pNetworkService && pNetworkService->IsConnected())
        ++m_shots;
}

void CharacterSync::SendLocal()
{
    const auto id = Red::GetGameSystem<NetworkWorldSystem>()->GetRemotePlayerId();
    if (!id)
        return;

    const auto pNetworkService = Core::Container::Get<NetworkService>();

    m_local.reset();
    if (!Red::CallVirtual(this, "ReadLocalState"))
        Failed("ReadLocalState");

    // A new character (after a reload of the save, say) hasn't been sent anything yet.
    if (m_sentId != *id)
        m_sent.reset();

    if (m_local && m_local != m_sent)
    {
        client::CharacterStateRequest request;
        request.set_id(*id);
        request.set_locomotion(m_local->Locomotion);
        request.set_upper_body(m_local->UpperBody);
        request.set_weapon_state(m_local->WeaponState);
        request.set_weapon(m_local->Weapon);
        if (pNetworkService->Send(request))
        {
            m_sent = m_local;
            m_sentId = *id;
        }
    }

    if (m_shots > 0)
    {
        client::CharacterShotRequest request;
        request.set_id(*id);
        request.set_count(m_shots);
        pNetworkService->Send(request);
        m_shots = 0;
    }
}

void CharacterSync::Show(flecs::entity aEntity, const EntityComponent& acEntity, RemoteStateComponent& aState)
{
    auto id = acEntity.Id;

    // In a car the seat's animations take over (VehicleSystem.reds puts the weapon away and clears the stances):
    // everything is shown again once out. Waiting to sit in one too: its player is seated already.
    if (aEntity.has<AttachedComponent>() || aEntity.has<WaitingSeatComponent>())
    {
        if (aState.Shown)
        {
            bool armed = false;
            bool aiming = false;
            float pitch = 0.f;
            if (!Red::CallVirtual(this, "ApplyAim", id, armed, aiming, pitch))
                Failed("ApplyAim");
        }
        aState.Shown = false;
        aState.WeaponInHand = false;
        aState.WeaponAttempts = 0;
        aState.InCombatMode = false;
        return;
    }

    if (!aState.Described)
    {
        bool described = false;
        if (!Red::CallVirtual(this, "DescribeAnimation", described, id))
        {
            Failed("DescribeAnimation");
            described = true; // not tried every run
        }
        aState.Described = described;
    }

    const bool refresh = ++aState.SinceRefresh >= kRefreshRuns;
    if (refresh)
    {
        aState.SinceRefresh = 0;
        aState.WeaponInHand = false; // checked again
    }

    Red::TweakDBID weapon(aState.Weapon);
    if (!aState.WeaponInHand)
    {
        bool holds = false;
        if (!Red::CallVirtual(this, "HoldsWeapon", holds, id, weapon))
            Failed("HoldsWeapon");

        if (holds)
            aState.WeaponInHand = true;
        else
        {
            // Animated only the first time after a change: should the character put it away again on its own, the
            // checks every refresh put it back directly, not with the animation every 2 s.
            if (aState.WeaponAttempts % kWeaponRetryRuns == 0)
            {
                bool animated = aState.WeaponAttempts == 0;
                if (!Red::CallVirtual(this, "HoldWeapon", id, weapon, animated))
                    Failed("HoldWeapon");
            }
            ++aState.WeaponAttempts;
        }
    }

    bool armed = aState.Weapon != 0;
    if (armed != aState.InCombatMode)
    {
        if (!Red::CallVirtual(this, "ApplyCombatMode", id, armed))
            Failed("ApplyCombatMode");
        aState.InCombatMode = armed;
    }

    const bool changed = !aState.Shown || armed != aState.ShownArmed || aState.Locomotion != aState.ShownLocomotion ||
                         aState.UpperBody != aState.ShownUpperBody || aState.WeaponState != aState.ShownWeaponState;
    if (changed || refresh)
    {
        auto locomotion = static_cast<int32_t>(aState.Locomotion);
        auto upperBody = static_cast<int32_t>(aState.UpperBody);
        auto weaponState = static_cast<int32_t>(aState.WeaponState);
        bool reloading = aState.ShownReloading;
        if (Red::CallVirtual(this, "ApplyStance", reloading, id, locomotion, upperBody, weaponState, armed, aState.ShownReloading))
        {
            aState.Shown = true;
            aState.ShownArmed = armed;
            aState.ShownLocomotion = aState.Locomotion;
            aState.ShownUpperBody = aState.UpperBody;
            aState.ShownWeaponState = aState.WeaponState;
            aState.ShownReloading = reloading;
        }
        else
            Failed("ApplyStance");
    }

    // Where they aim, while their weapon is out: every run, as they move and look around. Aiming down the sights, the
    // upper body's aim state (gamePSMUpperBodyStates.Aim).
    auto pitch = aState.AimPitch;
    bool aiming = aState.UpperBody == 6;
    if (!Red::CallVirtual(this, "ApplyAim", id, armed, aiming, pitch))
        Failed("ApplyAim");
}

void CharacterSync::Forget(flecs::entity aEntity)
{
    const auto* pEntity = aEntity.get<EntityComponent>();
    if (!pEntity || !aEntity.has<RemoteStateComponent>())
        return;

    auto id = pEntity->Id;
    if (!Red::CallVirtual(this, "ForgetAim", id))
        Failed("ForgetAim");
}

void CharacterSync::HandleCharacterState(const PacketEvent<server::NotifyCharacterState>& aMessage)
{
    const auto pWorld = Red::GetGameSystem<NetworkWorldSystem>();
    auto entity = pWorld->GetEntityByServerId(aMessage.get_id());
    if (!entity || !entity.is_alive())
        return;

    // Kept until the character exists here (it may still be spawning), then shown by Show.
    auto& state = entity.ensure<RemoteStateComponent>();
    if (state.Weapon != aMessage.get_weapon())
    {
        state.WeaponInHand = false;
        state.WeaponAttempts = 0;
    }

    // What arrives, in the log (the game's numbers: locomotion gamePSMLocomotionStates, 1 crouching; upper body
    // gamePSMUpperBodyStates, 6 aiming down the sights; weapon gamePSMRangedWeaponStates). Not every shot's.
    if (state.Locomotion != aMessage.get_locomotion() || state.UpperBody != aMessage.get_upper_body() ||
        state.Weapon != aMessage.get_weapon())
    {
        spdlog::info("[CharacterSync] character {:x}: locomotion {}, upper body {}, weapon {:x} ({})", aMessage.get_id(),
                     aMessage.get_locomotion(), aMessage.get_upper_body(), aMessage.get_weapon(),
                     aMessage.get_weapon_state());
    }
    state.Locomotion = aMessage.get_locomotion();
    state.UpperBody = aMessage.get_upper_body();
    state.WeaponState = aMessage.get_weapon_state();
    state.Weapon = aMessage.get_weapon();
}

void CharacterSync::HandleCharacterShot(const PacketEvent<server::NotifyCharacterShot>& aMessage)
{
    const auto pWorld = Red::GetGameSystem<NetworkWorldSystem>();
    const auto entity = pWorld->GetEntityByServerId(aMessage.get_id());
    if (!entity || !entity.is_alive() || entity.has<AttachedComponent>())
        return;

    const auto* pEntity = entity.get<EntityComponent>();
    if (!pEntity)
        return;

    auto id = pEntity->Id;
    auto count = static_cast<int32_t>(aMessage.get_count());
    if (!Red::CallVirtual(this, "ApplyShots", id, count))
        Failed("ApplyShots");
}
