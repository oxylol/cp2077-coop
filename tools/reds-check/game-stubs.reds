// Stand-in declarations of the game and Codeware script APIs that scripts/Cp2077Coop uses, so the redscript
// type checker can check our scripts without the game's script bundle (tools/reds-check/check.sh).
// Written by hand from public mod documentation; not a copy of game files. Keep in sync with what the bridge
// calls. Only signatures matter here.

// --- operators and globals (game) ---------------------------------------------------------------------------
native func OperatorAdd(a: script_ref<String>, b: script_ref<String>) -> String
native func OperatorAdd(a: Int32, b: Int32) -> Int32
native func OperatorAssignAdd(out a: Int32, b: Int32) -> Int32
native func OperatorSubtract(a: Float, b: Float) -> Float
native func OperatorGreater(a: Float, b: Float) -> Bool
native func OperatorLess(a: Float, b: Float) -> Bool
native func OperatorLess(a: Int32, b: Int32) -> Bool
native func OperatorEqual(a: Int32, b: Int32) -> Bool
native func OperatorEqual(a: Uint32, b: Uint32) -> Bool
native func OperatorLogicOr(a: Bool, b: Bool) -> Bool
native func OperatorLogicAnd(a: Bool, b: Bool) -> Bool
native func OperatorLogicNot(a: Bool) -> Bool

native func AbsF(a: Float) -> Float
native func FloatToStringPrec(value: Float, precision: Int32) -> String
native func FTLog(value: script_ref<String>)

// --- value types (game) -------------------------------------------------------------------------------------
public native struct Vector4 {
    public native let X: Float;
    public native let Y: Float;
    public native let Z: Float;
    public native let W: Float;

    public static native func Distance(a: Vector4, b: Vector4) -> Float
    public static native func ToRotation(dir: Vector4) -> EulerAngles
}

public native struct EulerAngles {
    public native let Roll: Float;
    public native let Pitch: Float;
    public native let Yaw: Float;

    public static native func ToQuat(rotation: EulerAngles) -> Quaternion
}

public native struct Quaternion {
    public native let i: Float;
    public native let j: Float;
    public native let k: Float;
    public native let r: Float;
}

public native struct EntityID {}
public native struct TweakDBID {}
public native struct ResRef {}

public native struct EngineTime {
    public static native func ToFloat(self: EngineTime) -> Float
}

// --- objects (game) -----------------------------------------------------------------------------------------
public native class IGameSystem extends IScriptable {}

public abstract native class ScriptableSystem extends IScriptable {
    public final native func GetGameInstance() -> GameInstance
    private func OnAttach() -> Void {}
}

public native class Entity extends IScriptable {
    public final native func GetWorldPosition() -> Vector4
    public final native func GetWorldForward() -> Vector4
}

public native class GameObject extends Entity {}
public native class PlayerPuppet extends GameObject {}

public native class TeleportationFacility extends IScriptable {
    public final native func Teleport(objectToTeleport: ref<GameObject>, position: Vector4, rotation: EulerAngles)
}

public native struct GameInstance {
    public static native func GetTeleportationFacility(self: GameInstance) -> ref<TeleportationFacility>
    public static native func GetSimTime(self: GameInstance) -> EngineTime
}

public func GetPlayer(game: GameInstance) -> ref<PlayerPuppet> {
    return null;
}

// --- Codeware -----------------------------------------------------------------------------------------------
public abstract native class CallbackSystemEvent extends IScriptable {}
public native class CallbackSystemHandler extends IScriptable {}

public native class GameSessionEvent extends CallbackSystemEvent {
    public native func IsRestored() -> Bool
    public native func IsPreGame() -> Bool
}

public native class CallbackSystem extends IGameSystem {
    public native func RegisterCallback(eventName: CName, target: ref<IScriptable>, function: CName, opt sticky: Bool) -> ref<CallbackSystemHandler>
}

public native class DynamicEntitySpec extends IScriptable {
    public native let recordID: TweakDBID;
    public native let templatePath: ResRef;
    public native let appearanceName: CName;
    public native let position: Vector4;
    public native let orientation: Quaternion;
    public native let persistState: Bool;
    public native let persistSpawn: Bool;
    public native let alwaysSpawned: Bool;
    public native let spawnInView: Bool;
    public native let active: Bool;
    public native let tags: array<CName>;
}

public native class DynamicEntitySystem extends IGameSystem {
    public native func CreateEntity(spec: ref<DynamicEntitySpec>) -> EntityID
    public native func DeleteEntity(id: EntityID) -> Bool
    public native func IsSpawned(id: EntityID) -> Bool
    public native func GetEntity(id: EntityID) -> ref<Entity>
}

@addMethod(GameInstance)
public static native func GetCallbackSystem() -> ref<CallbackSystem>

@addMethod(GameInstance)
public static native func GetDynamicEntitySystem() -> ref<DynamicEntitySystem>
