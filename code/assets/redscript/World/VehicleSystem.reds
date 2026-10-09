module CyberpunkMP.World

import Codeware.*
import CyberpunkMP.*

public native class VehicleSystem extends IScriptable {

    public native func OnVehicleEnter(entityID: EntityID, type: TweakDBID, sit_position: CName, vehicle_location: Vector4, vehicle_rotation: Quaternion) -> Void;
    public native func OnVehicleExit() -> Void;
    public native func OnVehicleReady(entityID: EntityID) -> Void;

    public func OnWorldAttached() -> Void {
        let callbackSystem = GameInstance.GetCallbackSystem();
        callbackSystem.RegisterCallback(n"Entity/Attached", this, n"OnEntityAttached")
            .AddTarget(DynamicEntityTarget.Tag(n"CyberpunkMP.Vehicle"));
    }

    // public func OnBeforeWorldDetach() -> Void {
    //     if IsDefined(this.m_callbackSystem) {
    //         this.m_callbackSystem.UnregisterCallback(n"Entity/Attached", this);
    //     }
    // }

    public func SpawnVehicle(type: TweakDBID, vehicle_location: Vector4, vehicle_rotation: Quaternion) -> EntityID {
        let entity_system = GameInstance.GetDynamicEntitySystem();

        let vehicle_spec = new DynamicEntitySpec();
        vehicle_spec.alwaysSpawned = true;
        vehicle_spec.recordID = type;
        vehicle_spec.position = vehicle_location;
        vehicle_spec.orientation = vehicle_rotation;
        vehicle_spec.persistState = false;
        vehicle_spec.persistSpawn = false;
        vehicle_spec.tags = [n"CyberpunkMP.Vehicle"];
        let vehicle_entity_id = entity_system.CreateEntity(vehicle_spec);

        return vehicle_entity_id;
    }

    private cb func OnEntityAttached(event: ref<EntityLifecycleEvent>) {
        LogChannel(n"DEBUG", "[VehicleSystem] OnEntityAttached");
        this.OnVehicleReady(event.GetEntity().GetEntityID());
    }

    // A vehicle another player drives here runs its engine; parked again, it doesn't.
    public func SetEngine(vehicle_id: EntityID, on: Bool) -> Void {
        let vehicle = GameInstance.FindEntityByID(GetGameInstance(), vehicle_id) as VehicleObject;
        if IsDefined(vehicle) {
            vehicle.TurnEngineOn(on);
        }
    }

    // Seats another player's character, as the game seats the player (vehicleTransition.script EnteringEvents): the
    // mount, the seat's animation set, and the seat's workspot, which animates sitting, driving and steering.
    public func EnterVehicle(character_id: EntityID, vehicle_id: EntityID, sit_position: CName) -> Bool {
        let game = GetGameInstance();
        let character = GameInstance.GetDynamicEntitySystem().GetEntity(character_id) as GameObject;
        let vehicle = GameInstance.FindEntityByID(game, vehicle_id) as VehicleObject;
        if !IsDefined(character) || !IsDefined(vehicle) {
            LogChannel(n"DEBUG", "[VehicleSystem] EnterVehicle: character or vehicle not found");
            return false;
        }

        let info: MountingInfo;
        info.parentId = vehicle_id;
        info.childId = character_id;
        info.slotId.id = sit_position;
        let data = new MountEventData();
        data.slotName = sit_position;
        data.mountParentEntityId = vehicle_id;
        data.isInstant = true;
        data.ignoreHLS = true;
        // The vehicle reads these (VehicleComponent.OnMountingEvent).
        let options = new MountEventOptions();
        options.alive = true;
        options.occupiedByNonFriendly = false;
        data.mountEventOptions = options;
        let request = new MountingRequest();
        request.lowLevelMountingInfo = info;
        request.preservePositionAfterMounting = true;
        request.mountData = data;
        GameInstance.GetMountingFacility(game).Mount(request);

        CoopClearForVehicle(character);
        let animVariables = VehicleComponent.SetAnimsetOverrideForPassenger(character, vehicle_id, sit_position, 1.0);
        let workspots = GameInstance.GetWorkspotSystem(game);
        let synchronized: array<EntityID>;
        workspots.StopNpcInWorkspot(character);
        let seated = workspots.MountToVehicle(vehicle, character, 0.0, 0.0, n"OccupantSlots", sit_position, synchronized, n"default", animVariables);

        // In the vehicle stance, as NPCs ride (NPCStatesComponent): its animations are the seat's, driving or riding.
        this.ApplyVehicleStance(character, true);
        return seated;
    }

    private func ApplyVehicleStance(character: ref<GameObject>, inVehicle: Bool) -> Void {
        let stance = new AnimFeature_NPCState();
        stance.state = inVehicle ? EnumInt(gamedataNPCStanceState.Vehicle) : EnumInt(gamedataNPCStanceState.Stand);
        AnimationControllerComponent.ApplyFeature(character, n"stanceState", stance);
        AnimationControllerComponent.SetAnimWrapperWeightOnOwnerAndItems(character, n"inVehicle", inVehicle ? 1.0 : 0.0);
    }

    // Takes another player's character out of its seat.
    public func ExitVehicle(character_id: EntityID) -> Bool {
        let game = GetGameInstance();
        let character = GameInstance.GetDynamicEntitySystem().GetEntity(character_id) as GameObject;
        if !IsDefined(character) {
            return false;
        }

        let mounting = GameInstance.GetMountingFacility(game);
        let info = mounting.GetMountingInfoSingleWithObjects(character);
        let vehicle = GameInstance.FindEntityByID(game, info.parentId) as VehicleObject;
        GameInstance.GetWorkspotSystem(game).UnmountFromVehicle(vehicle, character, true);
        if EntityID.IsDefined(info.parentId) {
            VehicleComponent.SetAnimsetOverrideForPassenger(character, info.parentId, info.slotId.id, 0.0);
            let data = new MountEventData();
            data.mountEventOptions = new MountEventOptions();
            let request = new UnmountingRequest();
            request.lowLevelMountingInfo = info;
            request.mountData = data;
            mounting.Unmount(request);
        }
        this.ApplyVehicleStance(character, false);

        // Upright, facing where it faced: in its seat it leaned with the car, and that tilt would stay.
        let facing = Vector4.ToRotation(Vector4.Normalize2D(character.GetWorldForward()));
        GameInstance.GetTeleportationFacility(game).Teleport(character, character.GetWorldPosition(), facing);
        return true;
    }

    // public func MountVehicle(parentID: EntityID, childId: EntityID, slot: CName) -> Void {
    //     let lowLevelMountingInfo: MountingInfo;
    //     let mountingRequest: ref<MountingRequest> = new MountingRequest();
    //     let mountData: ref<MountEventData> = new MountEventData();
    //     let mountOptions: ref<MountEventOptions> = new MountEventOptions();
    //     lowLevelMountingInfo.parentId = parentID;
    //     lowLevelMountingInfo.childId = childId;
    //     lowLevelMountingInfo.slotId.id = slot;
    //     mountingRequest.lowLevelMountingInfo = lowLevelMountingInfo;
    //     mountingRequest.preservePositionAfterMounting = true;
    //     mountingRequest.mountData = mountData;
    //     mountOptions.alive = false;
    //     mountOptions.occupiedByNonFriendly = false;
    //     mountingRequest.mountData.mountEventOptions = mountOptions;
    //     GameInstance.GetMountingFacility(GetGameInstance()).Mount(mountingRequest);
    // }
}

@wrapMethod(PlayerPuppet)
protected cb func OnMountingEvent(evt: ref<MountingEvent>) -> Bool {
    let result: Bool = wrappedMethod(evt);

    let entity_system = GameInstance.GetDynamicEntitySystem();

    let mounting_info = evt.request.lowLevelMountingInfo;
    let vehicle = entity_system.GetEntity(mounting_info.parentId) as VehicleObject;

    let type = vehicle.GetRecordID();
    let sit_position = mounting_info.slotId.id;
    let vehicle_location = vehicle.GetWorldPosition();
    let vehicle_rotation = vehicle.GetWorldOrientation();

    GameInstance.GetNetworkWorldSystem().GetVehicleSystem().OnVehicleEnter(mounting_info.parentId, type, sit_position, vehicle_location, vehicle_rotation);

    return result;
}

@wrapMethod(PlayerPuppet)
protected cb func OnUnmountingEvent(evt: ref<UnmountingEvent>) -> Bool {
    let result: Bool = wrappedMethod(evt);
    
    GameInstance.GetNetworkWorldSystem().GetVehicleSystem().OnVehicleExit();

    return result;
}