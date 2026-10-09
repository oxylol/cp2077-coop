
#include "VehicleSystem.h"

#include "App/Network/NetworkService.h"
#include "RED4ext/Scripting/Natives/Generated/game/Puppet.hpp"
#include "RED4ext/Scripting/Natives/gameIEntityStubSystem.hpp"
#include "RED4ext/Scripting/Natives/Generated/game/EntityStubComponentPS.hpp"
#include "RED4ext/Scripting/Natives/Generated/vehicle/WheeledBaseObject.hpp"

#include "NetworkWorldSystem.h"
#include "Game/Utils.h"
#include "App/Components/EntityComponent.h"
#include "App/Components/AttachedComponent.h"
#include "App/Components/SpawningComponent.h"
#include "App/Components/InterpolationComponent.h"
#include <RED4ext/Scripting/Natives/Generated/vehicle/MoveSystem.hpp>

// As InterpolationSystem.cpp switches it on for vehicles it moves.
static void SetSimpleMovement(Red::vehicle::IMoveSystem* apMoveSystem, const Red::EntityID& aEntityId, bool aEnabled)
{
    reinterpret_cast<void (*)(Red::vehicle::IMoveSystem*, const Red::EntityID&, bool)>(
        *(uintptr_t*)(*(uintptr_t*)apMoveSystem + 0x1F0))(apMoveSystem, aEntityId, aEnabled);
}

void VehicleSystem::OnWorldAttached(RED4ext::world::RuntimeScene* aScene)
{
    m_ready = true;
    Red::CallVirtual(this, "OnWorldAttached");
}

void VehicleSystem::OnAfterWorldDetach()
{
    m_ready = false;
}

void VehicleSystem::OnDisconnected()
{
    m_vehicleRemoteId = std::nullopt;
    m_vehicleGameId = std::nullopt;
    m_pendingMounts.clear();
}

void VehicleSystem::OnInitialize(const RED4ext::JobHandle& aJob)
{
    const auto pNetworkService = Core::Container::Get<NetworkService>();
    pNetworkService->RegisterHandler<&VehicleSystem::HandleVehicleLoadMessage>(this);
    pNetworkService->RegisterHandler<&VehicleSystem::HandleVehicleEnterMessage>(this);
    pNetworkService->RegisterHandler<&VehicleSystem::HandleVehicleExitMessage>(this);
    pNetworkService->RegisterHandler<&VehicleSystem::HandleVehicleControlMessage>(this);

    m_pSpawnVehicle = Red::Detail::GetFunction(GetType(), "SpawnVehicle");
    m_pEnterVehicle = Red::Detail::GetFunction(GetType(), "EnterVehicle");
    m_pExitVehicle = Red::Detail::GetFunction(GetType(), "ExitVehicle");
}

std::optional<uint64_t> VehicleSystem::GetVehicleRemoteId() const
{
    return m_vehicleRemoteId;
}

std::optional<Red::EntityID> VehicleSystem::GetVehicleGameId() const
{
    return m_vehicleGameId;
}

void VehicleSystem::OnVehicleEnter(Red::EntityID aVehicle, const Red::TweakDBID& aVehicleTdbid, Red::CName aName, const Red::Vector4& aPostion, const Red::Quaternion& aOrientation)
{
    spdlog::info("[VehicleSystem] OnVehicleEnter");
    const auto pNetworkService = Core::Container::Get<NetworkService>();
    if (!pNetworkService->IsConnected())
        return;

    const auto handle = Red::GetGameSystem<NetworkWorldSystem>();
    if (!handle->GetRemotePlayerId())
        return;

    client::EnterVehicleRequest request;
    request.set_id(*handle->GetRemotePlayerId());
    request.set_vehicle_id(aVehicleTdbid.value);
    request.set_sit_id(aName.hash);

    // The session knows the vehicle when it's a copy of another player's, or this player's own from before (since
    // they first drove it, HandleVehicleControlMessage): it's the same vehicle again, not a new one for the others.
    const auto serverVehicle = handle->FindEntity(aVehicle);
    if (serverVehicle)
    {
        request.set_remote_vehicle_id(serverVehicle);
        spdlog::info("[VehicleSystem] entering vehicle {:x} ({})", serverVehicle.id(), aName.ToString());
    }
    else
    {
        const auto cEntityRotation = eulerAngles(Game::ToGlm(aOrientation));

        common::Vector3 position;
        position.set_x(aPostion.X);
        position.set_y(aPostion.Y);
        position.set_z(aPostion.Z);
        request.set_position(position);
        request.set_rotation(cEntityRotation.z);
        spdlog::info("[VehicleSystem] entering a new vehicle ({})", aName.ToString());
    }

    m_vehicleGameId = aVehicle;
    m_vehicleRemoteId = std::nullopt; // until the session makes this player its driver

    pNetworkService->Send(request);
}

void VehicleSystem::OnVehicleExit()
{
    m_vehicleGameId = std::nullopt;
    m_vehicleRemoteId = std::nullopt;

    spdlog::info("[VehicleSystem] OnVehicleExit");
    const auto pNetworkService = Core::Container::Get<NetworkService>();
    if (!pNetworkService->IsConnected())
        return;

    client::ExitVehicleRequest request;

    const auto handle = Red::GetGameSystem<NetworkWorldSystem>();
    if (!handle->GetRemotePlayerId())
        return;
    request.set_id(*handle->GetRemotePlayerId());

    pNetworkService->Send(request);
}

bool VehicleSystem::HandleVehicleLoadMessage(const PacketEvent<server::NotifyVehicleLoad>& aMessage)
{
    spdlog::info("[VehicleSystem] HandleVehicleLoadMessage");
    const auto handle = Red::Handle(this);
    Red::EntityID id;
    Red::ScriptGameInstance game;

    Red::Vector4 position;
    position.X = aMessage.get_position().get_x();
    position.Y = aMessage.get_position().get_y();
    position.Z = aMessage.get_position().get_z();

    const auto eulerAngles = glm::vec3(0.f, 0.f, aMessage.get_rotation());
    const auto quat = glm::quat(eulerAngles);
    Red::Quaternion rotation = Game::ToRed(quat);

    if (!Red::Detail::CallFunctionWithArgs(m_pSpawnVehicle, handle, id, aMessage.get_tweak_id(), position, rotation))
        return false;

    if (!id.IsDynamic())
        return false;

    // spdlog::info("[VehicleSystem] * Spawned: {}, {}", aMessage.get_id(), id.hash);
    const auto worldSystem = Red::GetGameSystem<NetworkWorldSystem>();
    worldSystem->make_alive(aMessage.get_id()).emplace<SpawningComponent>(id);

    return true;
}

bool VehicleSystem::HandleVehicleEnterMessage(const PacketEvent<server::NotifyVehicleEnter>& aMessage)
{
    spdlog::info("[VehicleSystem] HandleVehicleEnterMessage: character {:x} into vehicle {:x}", aMessage.get_character_id(),
                 aMessage.get_vehicle_id());

    const auto worldSystem = Red::GetGameSystem<NetworkWorldSystem>();

    const auto sit = Red::CName(aMessage.get_sit_id());
    const auto character = worldSystem->GetEntityByServerId(aMessage.get_character_id());
    const auto vehicle = worldSystem->GetEntityByServerId(aMessage.get_vehicle_id());
    if (!character.is_alive() || !vehicle.is_alive())
    {
        spdlog::warn("[VehicleSystem] character {:x} or vehicle {:x} isn't known here", aMessage.get_character_id(),
                     aMessage.get_vehicle_id());
        return true;
    }

    if (const auto* pVehicle = vehicle.get<EntityComponent>())
    {
        DoMount(character, pVehicle->Id, sit);
    }
    else if (const auto* pSpawning = vehicle.get<SpawningComponent>())
    {
        // Already there: its OnVehicleReady came and went with nobody to seat.
        const auto id = pSpawning->Id;
        if (worldSystem->GetEntity(id))
        {
            Ready(vehicle, id);
            DoMount(character, id, sit);
        }
        else
            m_pendingMounts[id].push_back(aMessage);
    }

    return true;
}

void VehicleSystem::Ready(flecs::entity aVehicle, Red::EntityID aGameId)
{
    if (aVehicle.has<EntityComponent>())
        return;
    aVehicle.remove<SpawningComponent>();
    aVehicle.emplace<EntityComponent>(aGameId, true, nullptr);
}

void VehicleSystem::OnVehicleReady(const Red::EntityID& aVehicleEntityId)
{
    spdlog::info("[VehicleSystem] OnVehicleReady");

    const auto worldSystem = Red::GetGameSystem<NetworkWorldSystem>();
    const auto vehicle = worldSystem->FindEntity(aVehicleEntityId);
    if (!vehicle)
    {
        spdlog::info("[VehicleSystem] * Couldn't find vehicle: {}", aVehicleEntityId.hash);
        return;
    }
    Ready(vehicle, aVehicleEntityId);

    auto pending = m_pendingMounts.find(aVehicleEntityId);
    if (pending == m_pendingMounts.end())
        return;

    const auto messages = std::move(pending.value());
    m_pendingMounts.erase(pending);
    for (const auto& message : messages)
    {
        const auto character = worldSystem->GetEntityByServerId(message.get_character_id());
        if (character.is_alive())
            DoMount(character, aVehicleEntityId, Red::CName(message.get_sit_id()));
    }
}

bool VehicleSystem::HandleVehicleExitMessage(const PacketEvent<server::NotifyVehicleExit>& aMessage)
{
    spdlog::info("[VehicleSystem] HandleVehicleExitMessage");

    const auto worldSystem = Red::GetGameSystem<NetworkWorldSystem>();
    const auto handle = Red::Handle(this);
    bool res;
    const auto character = worldSystem->GetEntityIdByServerId(aMessage.get_character_id());
    Red::Detail::CallFunctionWithArgs(m_pExitVehicle, handle, res, character);

    auto characterEntity = worldSystem->GetEntityByServerId(aMessage.get_character_id());

    characterEntity.remove<AttachedComponent>();

    return true;
}

bool VehicleSystem::HandleVehicleControlMessage(const PacketEvent<server::NotifyVehicleControlAssigned>& aMessage)
{
    // This player drives it now; out already, nothing to do.
    if (!m_vehicleGameId)
        return true;

    m_vehicleRemoteId = aMessage.get_vehicle_id();
    spdlog::info("[VehicleSystem] driving vehicle {:x}", *m_vehicleRemoteId);

    // Known under the session's id from now on: getting back in, or someone else getting in, is the same vehicle for
    // everyone (one copy each, not one more every time). The others' moves don't move it while this player drives
    // (InterpolationSystem), and it's driven by its physics again: a copy was made kinematic for its previous driver.
    const auto worldSystem = Red::GetGameSystem<NetworkWorldSystem>();
    auto vehicle = worldSystem->GetEntityByServerId(*m_vehicleRemoteId);
    const bool own = !vehicle.is_alive(); // not a copy of another player's
    if (own)
        vehicle = worldSystem->make_alive(*m_vehicleRemoteId);
    Ready(vehicle, *m_vehicleGameId);
    if (own)
        vehicle.get_mut<EntityComponent>()->Owned = true;
    if (auto* pInterpolation = vehicle.get_mut<InterpolationComponent>())
        pInterpolation->TimePoints.clear();

    static Core::RawFunc<4039776020UL, void (*)(Red::vehicle::BaseObject*, bool)> SetIsPlayerControlled;
    static Core::RawFunc<1585713002UL, void (*)(Red::vehicle::BaseObject*, bool)> SetKinematic;
    if (const auto pVehicle = Red::Cast<Red::vehicle::BaseObject>(worldSystem->GetEntity(*m_vehicleGameId)))
    {
        if (const auto pMoveSystem = Red::GetGameSystem<Red::vehicle::IMoveSystem>())
            SetSimpleMovement(pMoveSystem, *m_vehicleGameId, false);
        SetKinematic(pVehicle, false);
        SetIsPlayerControlled(pVehicle, true);
    }

    return true;
}

void VehicleSystem::DoMount(flecs::entity aCharacter, Red::EntityID aVehicle, Red::CName aSit)
{
    const auto worldSystem = Red::GetGameSystem<NetworkWorldSystem>();
    const auto character = worldSystem->GetEntityIdByServerId(aCharacter);
    const auto handle = Red::Handle(this);
    bool res = false;

    // The script finds the vehicle itself: the player's own car isn't one of ours (GetEntity finds only ours).
    if (!Red::Detail::CallFunctionWithArgs(m_pEnterVehicle, handle, res, character, aVehicle, aSit) || !res)
        spdlog::warn("[VehicleSystem] seating character {} in vehicle {} failed", character.hash, aVehicle.hash);

    aCharacter.add<AttachedComponent>();

    // Moved by its driver's updates, unless this player drives it.
    const bool ours = m_vehicleRemoteId && m_vehicleGameId == aVehicle;
    const auto vehicle = Red::Cast<Red::vehicle::WheeledBaseObject>(worldSystem->GetEntity(aVehicle));
    if (vehicle && !ours)
    {
        // called from vehicle::actions::DriveAction::OnStart
        // static Core::RawFunc<4018412273UL, void (*)(Red::move::Component *, IMoveController &)> AttachLocomotionController
        static Core::RawFunc<4039776020UL, void (*)(Red::vehicle::BaseObject*, bool)> SetIsPlayerControlled;
        static Core::RawFunc<1620777158UL, void (*)(Red::vehicle::BaseObject*, uint32_t)> SetFlags;
        static Core::RawFunc<1585713002UL, void (*)(Red::vehicle::BaseObject*, bool)> SetKinematic;

        // AttachLocomotionController(component, controller);
        SetIsPlayerControlled(vehicle, false);
        // turn on engine
        reinterpret_cast<void (*)(Red::vehicle::WheeledBaseObject*, bool)>(*(uintptr_t*)(*(uintptr_t*)vehicle.instance + 0x328))(vehicle, true);
        vehicle->engineData->unk61 = 0;
        SetFlags(vehicle, 0x10);
        SetFlags(vehicle, 0x80);
        SetKinematic(vehicle, true);
    }
}

