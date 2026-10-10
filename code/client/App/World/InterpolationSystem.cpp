#include "InterpolationSystem.h"

#include <App/Components/AttachedComponent.h>
#include <App/Threading/ThreadService.h>
#include <App/World/PuppetRegistry.h>

#include "App/Network/NetworkService.h"
#include "RED4ext/Scripting/Natives/Generated/game/EntityStubComponentPS.hpp"
#include "RED4ext/Scripting/Natives/Generated/move/Component.hpp"
#include "RED4ext/Scripting/Natives/Generated/ent/AnimationControllerComponent.hpp"
#include <RED4ext/Scripting/Natives/Generated/vehicle/MoveSystem.hpp>
#include "RED4ext/Scripting/Natives/Generated/vehicle/BaseObject.hpp"
#include "RED4ext/Scripting/Natives/Generated/WorldPosition.hpp"
#include "RED4ext/Scripting/Natives/Vector3.hpp"

#include "Core/Hooking/HookingAgent.hpp"
#include "NetworkWorldSystem.h"
#include "App/Components/EntityComponent.h"
#include "Math/Math.h"
#include "App/Components/InterpolationComponent.h"
#include "App/Components/RemoteStateComponent.h"
#include "App/Components/SpawningComponent.h"
#include "Game/Utils.h"
#include "Game/Movement.h"
#include "Game/Animation/AnimationData.h"
#include "Game/Animation/MultiMovementController.h"

#include <Math/Spline.h>

inline void ResetWithNewTransform(Red::vehicle::BaseObject* self, const Red::WorldTransform& transform)
{
    reinterpret_cast<void (*)(Red::vehicle::BaseObject*, const Red::WorldTransform&)>(*(uintptr_t*)(*(uintptr_t*)self + 0x280))(self, transform);
}

inline void SetSimpleMovement(Red::vehicle::IMoveSystem* apMoveSystem, const Red::EntityID& aEntityId, bool enabled)
{
    reinterpret_cast<void (*)(Red::vehicle::IMoveSystem*, const Red::EntityID&, bool)>(*(uintptr_t*)(*(uintptr_t*)apMoveSystem + 0x1F0))(apMoveSystem, aEntityId, enabled);
}

void InterpolateEntity(flecs::entity aEntity, const EntityComponent& aEntityComponent, InterpolationComponent& aInterpolation, float aSimulationDelay, Red::vehicle::IMoveSystem* apMoveSystem)
{
    const float tick = NetworkWorldSystem::GetTick() - aSimulationDelay;

    while (!aInterpolation.TimePoints.empty())
    {
        auto future = *aInterpolation.TimePoints.begin();
        if (tick > future.Tick)
            aInterpolation.TimePoints.pop_front();
        else
            break;
    }

    if (aInterpolation.TimePoints.empty())
        return;

    const auto& first = aInterpolation.PreviousFrame;
    const auto& second = aInterpolation.TimePoints.front();

    // Calculate delta movement since last update
    auto ratio = 0.0001f;
    const auto tickDelta = static_cast<float>(second.Tick - first.Tick);
    if (tickDelta > 0.0)
    {
        ratio = 1.0f / tickDelta * (tick - first.Tick);
    }

    // A player's own vehicle is no stub of ours: it's checked below, as every vehicle is.
    if (!aEntityComponent.IsVehicle)
    {
        const auto pEntityStubSystem = Red::GetGameSystem<Red::game::IEntityStubSystem>();
        if (!pEntityStubSystem->FindStub(aEntityComponent.Id))
            return;
    }

    // Seated in a vehicle: the seat moves the character, until it's out. Waiting to sit (WaitingSeatComponent): it
    // stays where it stands, not walked into the seat its moves are from.
    if (aEntity.has<AttachedComponent>() || aEntity.has<WaitingSeatComponent>())
    {
        if (aEntityComponent.Controller)
            aEntityComponent.Controller->SetFrozen(true);
        return;
    }

    const glm::vec3 position{Lerp(first.Position, second.Position, ratio)};

    if (aEntityComponent.IsVehicle)
    {
        const auto pSystem = Red::GetGameSystem<NetworkWorldSystem>();
        // The vehicle this player drives goes where they drive it.
        if (pSystem->GetVehicleSystem()->GetVehicleRemoteId() == aEntity.id())
            return;

        // Moved only while another player sits at its wheel here: an update still on its way after they got out
        // would put it back under remote control (simple movement) with nothing left to release it, and whoever sat
        // at its wheel next couldn't drive it.
        if (!pSystem->GetVehicleSystem()->IsRemoteDriven(aEntityComponent.Id))
        {
            aInterpolation.TimePoints.clear();
            return;
        }

        const auto vehicle = Red::Cast<Red::vehicle::BaseObject>(pSystem->GetEntity(aEntityComponent.Id));
        if (vehicle)
        {
            const auto deltaAngleX{DeltaAngle(first.Rotation.x, second.Rotation.x, true) * ratio};
            const auto directionX = Mod(first.Rotation.x + deltaAngleX, 2.f * static_cast<float>(Pi));

            const auto deltaAngleY{DeltaAngle(first.Rotation.y, second.Rotation.y, true) * ratio};
            const auto directionY = Mod(first.Rotation.y + deltaAngleY, 2.f * static_cast<float>(Pi));

            const auto deltaAngleZ{DeltaAngle(first.Rotation.z, second.Rotation.z, true) * ratio};
            const auto directionZ = Mod(first.Rotation.z + deltaAngleZ, 2.f * static_cast<float>(Pi));

            const auto rot = glm::vec3(directionX, directionY, directionZ);

            auto transform = Red::WorldTransform();
            transform.Position = Red::WorldPosition(Red::Vector4{position.x, position.y, position.z, 0.f});
            transform.Orientation = Game::ToRed(glm::quat(rot));

            static Core::RawFunc<2933986803UL, void (*)(Red::vehicle::BaseObject*, const Red::WorldTransform&, float)> ForceMoveTo;
            static Core::RawFunc<457775029UL, void (*)(Red::vehicle::BaseObject*, const Red::WorldTransform&, bool)> ForceTransform;
            static Core::RawFunc<2592348558UL, void (*)(Red::vehicle::BaseObject*, uint32_t, bool)> EnablePhysics;

            SetSimpleMovement(apMoveSystem, aEntityComponent.Id, true);

            // modeled after DriveToPoint::OnUpdate_Internal, 3713079867
            if ((vehicle->physicsState & 0x100) == 0)
            {
                EnablePhysics(vehicle, 0x100, false);
            }

            ForceMoveTo(vehicle, transform, tick - aInterpolation.PreviousFrame.Tick);
            //ForceTransform(vehicle, transform, true);

            aInterpolation.PreviousFrame = InterpolationComponent::Timepoint{position, rot, 0.f, static_cast<uint64_t>(tick)};
        }
    }
    else
    {
        if (aEntityComponent.Controller)
        {
            aEntityComponent.Controller->SetFrozen(false);
            const auto speed{Lerp(first.Velocity, second.Velocity, ratio)};
            const auto deltaAngle{DeltaAngle(first.Rotation.z, second.Rotation.z, true) * ratio};
            const auto direction = Mod(first.Rotation.z + deltaAngle, 2.f * static_cast<float>(Pi));

            const auto pos = Red::Vector4{position.x, position.y, position.z, 0.f};
            aEntityComponent.Controller->SetTransform(pos, direction, speed);

            aInterpolation.PreviousFrame = InterpolationComponent::Timepoint{position, glm::vec3{0.f, 0.f, direction}, speed, static_cast<uint64_t>(tick)};
        }
    }
}

void InterpolationSystem::OnWorldAttached(RED4ext::world::RuntimeScene* aScene)
{
    spdlog::info("[InterpolationSystem] OnWorldAttached");

    static std::once_flag s_flag;
    std::call_once(s_flag, [this]()
    {
        const auto pSystem = Red::GetGameSystem<NetworkWorldSystem>();

        m_entityObserver = pSystem->observer<EntityComponent>()
           .event(flecs::OnAdd)
           .write<InterpolationComponent>()
           .each([](flecs::entity aEntity, EntityComponent&)
           {
               aEntity.add<InterpolationComponent>();
           });
    });

    m_ready = true;
}

void InterpolationSystem::OnAfterWorldDetach()
{
    spdlog::info("[InterpolationSystem] OnAfterWorldDetach");

    m_ready = false;
}

void InterpolationSystem::OnConnected()
{
    const auto pNetworkService = Core::Container::Get<NetworkService>();

    const float cSimulationDelay = 50.f + (1500.f / pNetworkService->GetServerSettings().get_update_rate());

    const auto pSystem = Red::GetGameSystem<NetworkWorldSystem>();
    const auto pMoveSystem = Red::GetGameSystem<RED4ext::vehicle::IMoveSystem>();

    m_interpolator = pSystem->system<const EntityComponent, InterpolationComponent>().each(
        [cSimulationDelay, pMoveSystem](flecs::entity aEntity, const EntityComponent& aEntityComponent, InterpolationComponent& aInterpolation)
        {
            InterpolateEntity(aEntity, aEntityComponent, aInterpolation, cSimulationDelay, pMoveSystem);
        });
}

void InterpolationSystem::OnDisconnected()
{
    if (m_interpolator)
        m_interpolator.destruct();
}

void InterpolationSystem::HandleNotifyEntityMove(const PacketEvent<server::NotifyEntityMove>& aMessage)
{
    const auto pSystem = Red::GetGameSystem<NetworkWorldSystem>();
    const auto entity = pSystem->GetEntityByServerId(aMessage.get_id());

    const glm::vec3 position{aMessage.get_position().get_x(), aMessage.get_position().get_y(), aMessage.get_position().get_z()};
    glm::vec3 rotation{0.f, 0.f, 0.f};

    if (aMessage.has_full_rotation())
        rotation = {aMessage.get_full_rotation().get_x(), aMessage.get_full_rotation().get_y(), aMessage.get_full_rotation().get_z()};
    else
        rotation = {0.f, 0.f, aMessage.get_rotation()};

    if (!entity)
        return;

    // Ours: the vehicle this player drives (moves from its previous driver may still be on their way).
    if (pSystem->GetVehicleSystem()->GetVehicleRemoteId() == aMessage.get_id())
        return;

    // Where a character aims, up or down (CharacterSync shows it).
    if (auto* pState = entity.get_mut<RemoteStateComponent>())
        pState->AimPitch = aMessage.get_aim_pitch();

    // A character still spawning in this game has no InterpolationComponent yet (it comes with the EntityComponent
    // once the game object exists), and a newcomer's first move arrives right after its spawn: start it here, so the
    // move waits for the puppet instead of reading through a null pointer (the host crashed on every join).
    auto& interpolation = entity.ensure<InterpolationComponent>();

    if (!interpolation.TimePoints.empty() && interpolation.TimePoints.back().Tick > aMessage.get_tick())
        return;

    interpolation.TimePoints.push_back(InterpolationComponent::Timepoint{position, rotation, aMessage.get_speed(), aMessage.get_tick()});
}

static Core::RawFunc<4018412273UL, float (*)(Red::move::Component*, MultiMovementController*)> AttachController;

void (*RealIdleController_SetAnimation)(Game::Controller*, AnimationData&);
void HookIdleController_SetAnimation(Game::Controller* apController, AnimationData& data)
{
    auto* pMoveComponent = apController->MoveComponent;
    Game::EntityPtr entity(pMoveComponent);
    if (const auto pOwner = Red::Cast<Red::GameObject>(entity.GetValuePtr()))
    {
        // This hook runs on the game's ANIMATION thread. Reading pOwner->tags here
        // walks a heap DynArray on an entity the main thread may still be building,
        // which raced and crashed the game shortly after a remote player spawned.
        // PuppetRegistry only touches the entity's 64-bit id.
        if (App::PuppetRegistry::Contains(pOwner->id.hash))
        {
            if (apController->m_type == MultiMovementController::kMulti)
                return;

            ThreadService::RunInMainThread([id = pOwner->id, pMoveComponent, apController]
            {
                const auto pSystem = Red::GetGameSystem<NetworkWorldSystem>();
                const auto entityQuery = pSystem->query<const EntityComponent>();
                auto flecsEntity = entityQuery.find([id](const EntityComponent& component) { return component.Id == id; });

                auto* pController = Red::Memory::New<MultiMovementController>();
                AttachController(pMoveComponent, pController);

                if (flecsEntity)
                {
                    flecsEntity.get_mut<EntityComponent>()->Controller = pController;
                }
                else
                {
                    const auto spawnQuery = pSystem->query<const SpawningComponent>();
                    flecsEntity = spawnQuery.find([id](const SpawningComponent& component) { return component.Id == id; });
                    if (flecsEntity)
                    {
                        flecsEntity.get_mut<SpawningComponent>()->Controller = pController;
                    }
                }                
            });

            return;
        }
    }

    RealIdleController_SetAnimation(apController, data);
}


void InterpolationSystem::OnInitialize(const RED4ext::JobHandle& aJob)
{
    Hook<Game::IdleController_SetAnimation>(&HookIdleController_SetAnimation, &RealIdleController_SetAnimation);

    const auto pNetworkService = Core::Container::Get<NetworkService>();
    pNetworkService->RegisterHandler<&InterpolationSystem::HandleNotifyEntityMove>(this);
}
