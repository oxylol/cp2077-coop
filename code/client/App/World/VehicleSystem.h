#pragma once

#include "Core/Stl.hpp"
#include "RED4ext/Scripting/Natives/Generated/Vector4.hpp"
#include "RED4ext/Scripting/Natives/Generated/Quaternion.hpp"
#include "RED4ext/Scripting/Natives/vehicleBaseObject.hpp"

struct VehicleSystem : RED4ext::IScriptable
{
    RTTI_IMPL_TYPEINFO(VehicleSystem)
    RTTI_IMPL_ALLOCATOR();

    void Update(uint64_t aTick);

    // A remote character out of its seat, if it's in one; before it goes (its player left, the session ended): a car
    // still holding a character that's gone crashed the game.
    void Unseat(flecs::entity aCharacter);

    void OnInitialize(const RED4ext::JobHandle& aJob);
    void OnWorldAttached(RED4ext::world::RuntimeScene* aScene);
    void OnAfterWorldDetach();
    void OnDisconnected();

    std::optional<uint64_t> GetVehicleRemoteId() const;
    std::optional<Red::EntityID> GetVehicleGameId() const;
    // Another player sits at its wheel here: their updates move it (InterpolationSystem), nothing else does.
    bool IsRemoteDriven(Red::EntityID aVehicle) const;
    // This player drives aVehicle (NetworkWorldSystem::UpdatePlayerLocation, as it sends its moves): every few seconds,
    // how it goes, into the log.
    void OnDriving(Red::vehicle::BaseObject* apVehicle);

protected:

    void OnVehicleEnter(Red::EntityID aVehicle, const Red::TweakDBID& aVehicleTdbid, Red::CName aName, const Red::Vector4& aPostion, const Red::Quaternion& aOrientation);
    void OnVehicleExit();
    void OnVehicleReady(const Red::EntityID& vehicle);
    void Log(const Red::CString& acText);
    // Another player's character sits in it here (whatever the seat).
    bool HasRemoteCharacters(Red::EntityID aVehicle);
    // Someone sits at its wheel here: this player, or another player's character.
    bool HasDriver(Red::EntityID aVehicle) const;
    // The characters waiting to sit on its passenger side (WaitingSeatComponent) are seated: someone took its wheel.
    void SeatWaitingPassengers(Red::EntityID aVehicle);

    bool HandleVehicleLoadMessage(const PacketEvent<server::NotifyVehicleLoad>& aMessage);
    bool HandleVehicleEnterMessage(const PacketEvent<server::NotifyVehicleEnter>& aMessage);
    bool HandleVehicleExitMessage(const PacketEvent<server::NotifyVehicleExit>& aMessage);
    bool HandleVehicleControlMessage(const PacketEvent<server::NotifyVehicleControlAssigned>& aMessage);
    bool HandleVehicleCreatedMessage(const PacketEvent<server::NotifyVehicleCreated>& aMessage);
    // This game's own vehicle, known to the session as aServerId.
    static void RegisterOwnVehicle(uint64_t aServerId, Red::EntityID aGameId);
    void SetVehicleEngine(Red::EntityID aVehicle, bool aOn); // VehicleSystem.reds SetEngine

    void DoMount(flecs::entity aCharacter, Red::EntityID aVehicle, Red::CName aSit);
    // The vehicle's game object exists: it's moved and seated in from now on.
    static void Ready(flecs::entity aVehicle, Red::EntityID aGameId);
    // Its remote driver got out (aParked: its engine stops) or this player took the wheel: driven by its physics again.
    void ReleaseRemoteDriving(Red::EntityID aVehicle, bool aParked);

private:
    bool m_ready{false};
    Core::Map<Red::EntityID, Vector<PacketEvent<server::NotifyVehicleEnter>>> m_pendingMounts;
    Red::CBaseFunction* m_pSpawnVehicle;
    Red::CBaseFunction* m_pEnterVehicle;
    Red::CBaseFunction* m_pExitVehicle;
    std::optional<uint64_t> m_vehicleRemoteId;
    std::optional<Red::EntityID> m_vehicleGameId;
    bool m_vehicleDriverSeat{false}; // this player sits at its wheel
    // Vehicles here with another player at the wheel: kinematic, moved by that player's updates (DoMount).
    Core::Set<Red::EntityID> m_remoteDriven;
    uint64_t m_lastDrivingLog{0};
};

RTTI_DEFINE_CLASS(VehicleSystem, { 
    RTTI_ALIAS("CyberpunkMP.World.VehicleSystem");
    RTTI_METHOD(OnVehicleEnter);
    RTTI_METHOD(OnVehicleExit);
    RTTI_METHOD(OnVehicleReady);
    RTTI_METHOD(Log);
    RTTI_METHOD(HasRemoteCharacters);
});