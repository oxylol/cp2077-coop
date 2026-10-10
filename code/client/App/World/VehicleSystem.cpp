
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
    m_remoteDriven.clear();
}

void VehicleSystem::OnInitialize(const RED4ext::JobHandle& aJob)
{
    const auto pNetworkService = Core::Container::Get<NetworkService>();
    pNetworkService->RegisterHandler<&VehicleSystem::HandleVehicleLoadMessage>(this);
    pNetworkService->RegisterHandler<&VehicleSystem::HandleVehicleEnterMessage>(this);
    pNetworkService->RegisterHandler<&VehicleSystem::HandleVehicleExitMessage>(this);
    pNetworkService->RegisterHandler<&VehicleSystem::HandleVehicleControlMessage>(this);
    pNetworkService->RegisterHandler<&VehicleSystem::HandleVehicleCreatedMessage>(this);

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

bool VehicleSystem::IsRemoteDriven(Red::EntityID aVehicle) const
{
    return m_remoteDriven.count(aVehicle) != 0;
}

void VehicleSystem::OnDriving(Red::vehicle::BaseObject* apVehicle)
{
    const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count());
    if (m_lastDrivingLog && now - m_lastDrivingLog < 3000)
        return;
    m_lastDrivingLog = now;

    const auto speed = apVehicle->rigidBody ? apVehicle->rigidBody->velocity.Magnitude() : 0.f;
    spdlog::info("[Driving] {:x}: {:.1f} m/s, throttle {:.2f}, brake {:.2f}, steering {:.2f}, handbrake {:.2f}, "
                 "physics 0x{:x}",
                 m_vehicleRemoteId.value_or(0), speed, apVehicle->acceleration, apVehicle->deceleration,
                 apVehicle->turnInput, apVehicle->handbrake, apVehicle->physicsState);
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
        request.set_game_vehicle(aVehicle.hash);
        spdlog::info("[VehicleSystem] entering a new vehicle {} ({})", aVehicle.hash, aName.ToString());
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

void VehicleSystem::Log(const Red::CString& acText)
{
    spdlog::info("[VehicleSystem.reds] {}", acText.c_str());
}

bool VehicleSystem::HasRemoteCharacters(Red::EntityID aVehicle)
{
    bool aboard = false;
    Red::GetGameSystem<NetworkWorldSystem>()->each([&aboard, aVehicle](flecs::entity, const AttachedComponent& acAttached) {
        aboard = aboard || acAttached.Vehicle == aVehicle;
    });
    return aboard;
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
    spdlog::info("[VehicleSystem] HandleVehicleExitMessage: character {:x}", aMessage.get_character_id());

    // Out before the vehicle was even there to seat them in: not seated later.
    for (auto it = m_pendingMounts.begin(); it != m_pendingMounts.end(); ++it)
    {
        auto& messages = it.value();
        std::erase_if(messages, [&](const auto& acMount) { return acMount.get_character_id() == aMessage.get_character_id(); });
    }

    const auto worldSystem = Red::GetGameSystem<NetworkWorldSystem>();
    auto characterEntity = worldSystem->GetEntityByServerId(aMessage.get_character_id());
    if (characterEntity.is_alive())
        Unseat(characterEntity);

    return true;
}

void VehicleSystem::Unseat(flecs::entity aCharacter)
{
    const auto* pAttached = aCharacter.get<AttachedComponent>();
    if (!pAttached)
        return;
    const auto seat = *pAttached;

    const auto worldSystem = Red::GetGameSystem<NetworkWorldSystem>();
    const auto handle = Red::Handle(this);
    bool res = false;
    const auto character = worldSystem->GetEntityIdByServerId(aCharacter);
    Red::Detail::CallFunctionWithArgs(m_pExitVehicle, handle, res, character);

    if (seat.Driver)
        ReleaseRemoteDriving(seat.Vehicle, true);
    aCharacter.remove<AttachedComponent>();
}

void VehicleSystem::ReleaseRemoteDriving(Red::EntityID aVehicle, bool aParked)
{
    if (!m_remoteDriven.erase(aVehicle))
        return;

    // Parked again: a car like any other, until someone drives it. Updates still on their way don't move it.
    static Core::RawFunc<1585713002UL, void (*)(Red::vehicle::BaseObject*, bool)> SetKinematic;
    const auto worldSystem = Red::GetGameSystem<NetworkWorldSystem>();
    if (auto vehicle = worldSystem->FindEntity(aVehicle))
    {
        if (auto* pInterpolation = vehicle.get_mut<InterpolationComponent>())
            pInterpolation->TimePoints.clear();
    }
    if (const auto pVehicle = Red::Cast<Red::vehicle::BaseObject>(worldSystem->GetEntity(aVehicle)))
    {
        if (const auto pMoveSystem = Red::GetGameSystem<Red::vehicle::IMoveSystem>())
            SetSimpleMovement(pMoveSystem, aVehicle, false);
        SetKinematic(pVehicle, false);
    }
    if (aParked)
        SetVehicleEngine(aVehicle, false);
    spdlog::info("[VehicleSystem] vehicle {} has no remote driver any more", aVehicle.hash);
}

bool VehicleSystem::HandleVehicleCreatedMessage(const PacketEvent<server::NotifyVehicleCreated>& aMessage)
{
    spdlog::info("[VehicleSystem] vehicle {} is {:x} in the session", aMessage.get_game_vehicle(), aMessage.get_vehicle_id());
    RegisterOwnVehicle(aMessage.get_vehicle_id(), Red::EntityID(aMessage.get_game_vehicle()));
    return true;
}

void VehicleSystem::RegisterOwnVehicle(uint64_t aServerId, Red::EntityID aGameId)
{
    // Known under the session's id from now on: getting back in, sliding over to the other seat, or someone else
    // getting in, is the same vehicle for everyone (one copy each, not one more every time).
    const auto worldSystem = Red::GetGameSystem<NetworkWorldSystem>();
    auto vehicle = worldSystem->GetEntityByServerId(aServerId);
    if (vehicle.is_alive())
        return;
    vehicle = worldSystem->make_alive(aServerId);
    Ready(vehicle, aGameId);
    vehicle.get_mut<EntityComponent>()->Owned = true; // this game's own, not a copy of another player's
}

bool VehicleSystem::HandleVehicleControlMessage(const PacketEvent<server::NotifyVehicleControlAssigned>& aMessage)
{
    // This player drives it now; out already, nothing to do.
    if (!m_vehicleGameId)
        return true;

    m_vehicleRemoteId = aMessage.get_vehicle_id();
    spdlog::info("[VehicleSystem] driving vehicle {:x}{}", *m_vehicleRemoteId,
                 m_remoteDriven.count(*m_vehicleGameId) ? " (still driven by another player here)" : "");
    m_lastDrivingLog = 0;

    // Normally known already (HandleVehicleCreatedMessage). The others' moves don't move it while this player drives
    // (InterpolationSystem).
    RegisterOwnVehicle(*m_vehicleRemoteId, *m_vehicleGameId);
    const auto worldSystem = Red::GetGameSystem<NetworkWorldSystem>();
    if (auto* pInterpolation = worldSystem->GetEntityByServerId(*m_vehicleRemoteId).get_mut<InterpolationComponent>())
        pInterpolation->TimePoints.clear();

    // Normally released when its previous driver got out (HandleVehicleExitMessage); only if that hasn't arrived yet.
    // Nothing more is touched here than what this game did to the car itself: the game is still busy seating this
    // player (or, after sliding over from the passenger seat, about to get them out).
    ReleaseRemoteDriving(*m_vehicleGameId, false);

    return true;
}

void VehicleSystem::DoMount(flecs::entity aCharacter, Red::EntityID aVehicle, Red::CName aSit)
{
    const auto worldSystem = Red::GetGameSystem<NetworkWorldSystem>();
    const auto character = worldSystem->GetEntityIdByServerId(aCharacter);
    const auto handle = Red::Handle(this);
    bool res = false;

    spdlog::info("[VehicleSystem] seating character {} in {} of vehicle {}{}", character.hash, aSit.ToString(), aVehicle.hash,
                 m_remoteDriven.count(aVehicle) ? " (driven by another player)" : "");

    // The script finds the vehicle itself: the player's own car isn't one of ours (GetEntity finds only ours).
    if (!Red::Detail::CallFunctionWithArgs(m_pEnterVehicle, handle, res, character, aVehicle, aSit) || !res)
        spdlog::warn("[VehicleSystem] seating character {} in vehicle {} failed", character.hash, aVehicle.hash);

    // Only a driver drives it: a passenger leaves the vehicle as it is (prepared for driving with nobody at the wheel,
    // the game crashed when someone got in on the passenger side).
    const bool driver = aSit == Red::CName("seat_front_left");
    aCharacter.set<AttachedComponent>({aVehicle, driver});
    if (!driver)
        return;

    // Moved by its driver's updates from now on, unless this player drives it.
    const bool ours = m_vehicleRemoteId && m_vehicleGameId == aVehicle;
    const auto vehicle = Red::Cast<Red::vehicle::WheeledBaseObject>(worldSystem->GetEntity(aVehicle));
    if (vehicle && !ours && m_remoteDriven.count(aVehicle) == 0)
    {
        m_remoteDriven.insert(aVehicle);

        // Kinematic (InterpolationSystem moves it), engine running: only what's undone when the driver gets out
        // (ReleaseRemoteDriving). The flags and engine state CyberpunkMP also set here (DriveAction::OnStart's, as the
        // game's AI driving sets them) stayed on the car, and the game crashed when someone sat in it with nobody at
        // the wheel; the last of them, "not the player's to drive", left a car this player had driven themselves
        // undrivable, and crashed the game the same way (seating someone on the passenger side of it, empty, after
        // another player drove it: every time, and only in a car this player had driven).
        static Core::RawFunc<1585713002UL, void (*)(Red::vehicle::BaseObject*, bool)> SetKinematic;
        SetKinematic(vehicle, true);
        SetVehicleEngine(aVehicle, true);
    }
}

void VehicleSystem::SetVehicleEngine(Red::EntityID aVehicle, bool aOn)
{
    if (!Red::CallVirtual(this, "SetEngine", aVehicle, aOn))
        spdlog::warn("[VehicleSystem] VehicleSystem.reds SetEngine failed");
}

