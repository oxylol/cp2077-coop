module CyberpunkMP.World

import Codeware.*
import CyberpunkMP.*

public native class VehicleSystem extends IScriptable {

    public native func OnVehicleEnter(entityID: EntityID, type: TweakDBID, sit_position: CName, vehicle_location: Vector4, vehicle_rotation: Quaternion) -> Void;
    public native func OnVehicleExit() -> Void;
    public native func OnVehicleReady(entityID: EntityID) -> Void;
    // A line in the mod's log (CyberpunkCoop.log).
    public native func Log(text: String) -> Void;

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

        this.Log("seating: mount " + NameToString(sit_position));
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

        this.Log("seating: clear weapon, animset");
        CoopClearForVehicle(character);
        CoopBlockReactions(character);
        let animVariables = VehicleComponent.SetAnimsetOverrideForPassenger(character, vehicle_id, sit_position, 1.0);
        let workspots = GameInstance.GetWorkspotSystem(game);
        let synchronized: array<EntityID>;
        this.Log("seating: workspot");
        workspots.StopNpcInWorkspot(character);
        let seated = workspots.MountToVehicle(vehicle, character, 0.0, 0.0, n"OccupantSlots", sit_position, synchronized, n"default", animVariables);

        // In the vehicle stance, as NPCs ride (NPCStatesComponent): its animations are the seat's, driving or riding.
        this.Log("seating: stance");
        this.ApplyVehicleStance(character, true);
        this.Log("seating: done");
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

        // The car is told the seat empties, as the game's own exits tell it (vehicleTransition.script
        // StartLeavingVehicle, aiVehicle.script): without it, it kept the character in that seat, and crashed when the
        // same character sat in another seat with nobody at the wheel, or once the character was gone.
        if IsDefined(vehicle) {
            let leaving = new VehicleStartedMountingEvent();
            leaving.slotID = info.slotId.id;
            leaving.isMounting = false;
            leaving.character = character;
            leaving.instant = true;
            vehicle.QueueEvent(leaving);
            // "NoDriver" too, as the game's exits send it, once this exit is through: the same character may move
            // to another seat of this car right away (its player got out on the other side).
            if VehicleComponent.IsDriverSlot(info.slotId.id) {
                let noDriver = new CoopNoDriverCallback();
                noDriver.vehicle = vehicle;
                GameInstance.GetDelaySystem(game).DelayCallback(noDriver, 0.2);
            }
        }
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

// The other players' characters here are stand-ins, seated and moved by their players' games, not passengers for the
// car's own logic to order out, scare or count: told "ExitVehicle", their AI, which never seated them, can't get them
// out, and the game crashed (also only the car's driver getting out, with one of them riding along).
public static func CoopIsStandIn(object: wref<GameObject>) -> Bool {
    return IsDefined(object) && GameInstance.GetDynamicEntitySystem().IsTagged(object.GetEntityID(), n"CyberpunkMP.Puppet");
}

public static func CoopRemoveStandIns(objects: script_ref<array<wref<GameObject>>>) -> Void {
    let i = ArraySize(Deref(objects)) - 1;
    while i >= 0 {
        if CoopIsStandIn(Deref(objects)[i]) {
            ArrayErase(Deref(objects), i);
        }
        i -= 1;
    }
}

@wrapMethod(VehicleComponent)
public final static func GetAllPassengers(gi: GameInstance, vehicleID: EntityID, includeTrunkBody: Bool, passengers: script_ref<array<wref<GameObject>>>) -> Void {
    wrappedMethod(gi, vehicleID, includeTrunkBody, passengers);
    CoopRemoveStandIns(passengers);
}

@wrapMethod(VehicleComponent)
public final static func CheckIfPassengersCanLeaveCar(gi: GameInstance, vehicleID: EntityID, passengersCanLeaveCar: script_ref<array<wref<GameObject>>>, passengersCantLeaveCar: script_ref<array<wref<GameObject>>>) -> Void {
    wrappedMethod(gi, vehicleID, passengersCanLeaveCar, passengersCantLeaveCar);
    CoopRemoveStandIns(passengersCanLeaveCar);
    CoopRemoveStandIns(passengersCantLeaveCar);
}


// The car's own logic and another player's character in it: what's said about it, for the log.
public static func CoopInSession() -> Bool {
    let ui = GameInstance.GetBlackboardSystem(GetGameInstance()).Get(GetAllBlackboardDefs().UIGameData);
    return IsDefined(ui) && ui.GetBool(GetAllBlackboardDefs().UIGameData.UIMultiplayerConnectedToServer);
}

public static func CoopLog(text: String) -> Void {
    let vehicles = GameInstance.GetNetworkWorldSystem().GetVehicleSystem();
    if IsDefined(vehicles) {
        vehicles.Log(text);
    }
}

public static func CoopDescribe(object: wref<GameObject>) -> String {
    if !IsDefined(object) {
        return "nobody";
    }
    if object.IsPlayer() {
        return "the player";
    }
    if CoopIsStandIn(object) {
        return "a co-op character";
    }
    return "an NPC";
}

// Another player's character sits in the vehicle (whatever the seat).
public static func CoopStandInsAboard(vehicle: wref<VehicleObject>) -> Bool {
    if !IsDefined(vehicle) {
        return false;
    }
    let mounts = GameInstance.GetMountingFacility(vehicle.GetGame()).GetMountingInfoMultipleWithIds(vehicle.GetEntityID());
    let i = 0;
    while i < ArraySize(mounts) {
        if GameInstance.GetDynamicEntitySystem().IsTagged(mounts[i].childId, n"CyberpunkMP.Puppet") {
            return true;
        }
        i += 1;
    }
    return false;
}

// "NoDriver" for a car another player's character got out of the driver seat of, unless someone sits at its wheel
// again, or it still carries another player's character (VehicleComponent.SendAIEvent below).
public class CoopNoDriverCallback extends DelayCallback {
    public let vehicle: wref<VehicleObject>;

    public func Call() -> Void {
        if !IsDefined(this.vehicle) {
            return;
        }
        if IsDefined(VehicleComponent.GetDriverMounted(this.vehicle.GetGame(), this.vehicle.GetEntityID())) {
            return;
        }
        if CoopStandInsAboard(this.vehicle) {
            CoopLog("car: no NoDriver, another player's character still rides in it");
            return;
        }
        let noDriver = new AIEvent();
        noDriver.name = n"NoDriver";
        this.vehicle.QueueEvent(noDriver);
    }
}

// The driver got out: the car tells its AI "NoDriver", which acts on the passengers left. Another player's character
// among them was never seated by that AI: the game died, every time, right after the player got out of the driver seat
// with one of them still on the passenger side. Their own player's game says when they get out.
@wrapMethod(VehicleComponent)
private final func SendAIEvent(eventName: CName) -> Void {
    if Equals(eventName, n"NoDriver") && CoopStandInsAboard(this.GetVehicle()) {
        CoopLog("car: no NoDriver, another player's character rides in it");
        return;
    }
    wrappedMethod(eventName);
}

@wrapMethod(VehicleComponent)
protected cb func OnMountingEvent(evt: ref<MountingEvent>) -> Bool {
    let child = GameInstance.FindEntityByID(this.GetVehicle().GetGame(), evt.request.lowLevelMountingInfo.childId) as GameObject;
    if CoopInSession() && (CoopIsStandIn(child) || IsDefined(child) && child.IsPlayer() || CoopStandInsAboard(this.GetVehicle())) {
        CoopLog("car: " + CoopDescribe(child) + " in " + NameToString(evt.request.lowLevelMountingInfo.slotId.id));
    }
    return wrappedMethod(evt);
}

@wrapMethod(VehicleComponent)
protected cb func OnUnmountingEvent(evt: ref<UnmountingEvent>) -> Bool {
    let child = GameInstance.FindEntityByID(this.GetVehicle().GetGame(), evt.request.lowLevelMountingInfo.childId) as GameObject;
    let logged = CoopInSession() && (CoopIsStandIn(child) || IsDefined(child) && child.IsPlayer() || CoopStandInsAboard(this.GetVehicle()));
    if logged {
        CoopLog("car: " + CoopDescribe(child) + " out of " + NameToString(evt.request.lowLevelMountingInfo.slotId.id));
    }
    let result = wrappedMethod(evt);
    if logged {
        CoopLog("car: " + CoopDescribe(child) + " out, done");
    }
    return result;
}

@wrapMethod(VehicleComponent)
protected cb func OnVehicleStartedMountingEvent(evt: ref<VehicleStartedMountingEvent>) -> Bool {
    if CoopInSession() && (CoopIsStandIn(evt.character) || IsDefined(evt.character) && evt.character.IsPlayer() || CoopStandInsAboard(this.GetVehicle())) {
        CoopLog("car: " + CoopDescribe(evt.character) + (evt.isMounting ? " getting into " : " getting out of ") + NameToString(evt.slotID));
    }
    return wrappedMethod(evt);
}

// The player gets out where the side seat's taken: teleported out, the car pushed aside.
@wrapMethod(VehicleEventsTransition)
protected final func ExitWithTeleport(stateContext: ref<StateContext>, scriptInterface: ref<StateGameScriptInterface>, validUnmountDirection: vehicleUnmountPosition, opt moveVehicle: Bool, opt skipUnmount: Bool) -> Void {
    if CoopInSession() {
        CoopLog("car: the player gets out by teleport" + (moveVehicle ? ", the car moved aside" : ""));
    }
    wrappedMethod(stateContext, scriptInterface, validUnmountDirection, moveVehicle, skipUnmount);
}
