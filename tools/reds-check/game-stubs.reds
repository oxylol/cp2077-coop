// Stand-in declarations of the game and Codeware script APIs that scripts/Cp2077Coop uses, so the redscript
// type checker can check our scripts without the game's script bundle (tools/reds-check/check.sh).
// Written by hand from public mod documentation; not a copy of game files. Keep in sync with what the bridge
// calls. Only signatures matter here.

// --- operators and globals (game) ---------------------------------------------------------------------------
native func OperatorAdd(a: script_ref<String>, b: script_ref<String>) -> String
native func OperatorAdd(a: Int32, b: Int32) -> Int32
native func OperatorAssignAdd(out a: Int32, b: Int32) -> Int32
native func OperatorSubtract(a: Float, b: Float) -> Float
native func OperatorNeg(a: Float) -> Float
native func OperatorGreater(a: Float, b: Float) -> Bool
native func OperatorLess(a: Float, b: Float) -> Bool
native func OperatorLess(a: Int32, b: Int32) -> Bool
native func OperatorGreater(a: Int32, b: Int32) -> Bool
native func OperatorGreaterEqual(a: Float, b: Float) -> Bool
native func OperatorGreaterEqual(a: Int32, b: Int32) -> Bool
native func OperatorEqual(a: Int32, b: Int32) -> Bool
native func OperatorNotEqual(a: Int32, b: Int32) -> Bool
native func OperatorAnd(a: Int32, b: Int32) -> Int32
native func OperatorAdd(a: Vector4, b: Vector4) -> Vector4
native func OperatorMultiply(a: Vector4, b: Float) -> Vector4
native func OperatorEqual(a: Uint32, b: Uint32) -> Bool
native func OperatorLogicOr(a: Bool, b: Bool) -> Bool
native func OperatorLogicAnd(a: Bool, b: Bool) -> Bool
native func OperatorLogicNot(a: Bool) -> Bool

native func AbsF(a: Float) -> Float
native func FloatToStringPrec(value: Float, precision: Int32) -> String
native func FTLog(value: script_ref<String>)
native func StrLen(str: String) -> Int32

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

public abstract native class TDBID {
    public static native func Create(str: String) -> TweakDBID
}

public native struct WorldPosition {
    public static native func SetVector4(self: script_ref<WorldPosition>, vector: Vector4)
}

// AI (spike S3 dump: AIMoveToCommand properties, AIHumanComponent.SendCommand/CancelCommand)
public native struct AIPositionSpec {
    public static native func SetWorldPosition(self: script_ref<AIPositionSpec>, worldPosition: WorldPosition)
}

enum moveMovementType {
    Walk = 0,
    Run = 1,
    Sprint = 2,
    Strafe = 3
}
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

public abstract native class AICommand extends IScriptable {}
public abstract native class AIMoveCommand extends AICommand {}

public native class AIMoveToCommand extends AIMoveCommand {
    public native let movementTarget: AIPositionSpec;
    public native let rotateEntityTowardsFacingTarget: Bool;
    public native let facingTarget: AIPositionSpec;
    public native let movementType: moveMovementType;
    public native let ignoreNavigation: Bool;
    public native let useStart: Bool;
    public native let useStop: Bool;
    public native let desiredDistanceFromTarget: Float;
    public native let finishWhenDestinationReached: Bool;
}

public native class AIHumanComponent extends IScriptable {
    public final native func SendCommand(cmd: ref<AICommand>) -> Bool
    public final native func CancelCommand(cmd: ref<AICommand>) -> Bool
}

public native class AttitudeAgent extends IScriptable {
    public final native func SetAttitudeGroup(attitudeGroup: CName)
}

public native class TimeDilatable extends Entity {
    public final native func SetIndividualTimeDilation(reason: CName, dilation: Float, opt duration: Float, opt easeInCurve: CName, opt easeOutCurve: CName, opt ignoreGlobalDilation: Bool, opt useRealTime: Bool)
    public final native func UnsetIndividualTimeDilation(opt easeOutCurve: CName)
}

public native class GameObject extends TimeDilatable {
    public final native func GetAttitudeAgent() -> ref<AttitudeAgent>
}

public native class gamePuppet extends GameObject {
    public final native func GetResolvedGenderName() -> CName
}

public class ScriptedPuppet extends gamePuppet {
    public final func GetAIControllerComponent() -> ref<AIHumanComponent> {
        return null;
    }
}

public class PlayerPuppet extends ScriptedPuppet {}

public native class TimeSystem extends IScriptable {
    public final native func SetTimeDilation(reason: CName, dilation: Float, opt duration: Float, opt easeInCurve: CName, opt easeOutCurve: CName, opt listener: ref<IScriptable>)
    public final native func UnsetTimeDilation(reason: CName, opt easeOutCurve: CName)
    public final native func SetIgnoreTimeDilationOnLocalPlayerZero(ignore: Bool)
}

public native class TeleportationFacility extends IScriptable {
    public final native func Teleport(objectToTeleport: ref<GameObject>, position: Vector4, rotation: EulerAngles)
}

public native struct GameInstance {
    public static native func GetTeleportationFacility(self: GameInstance) -> ref<TeleportationFacility>
    public static native func GetSimTime(self: GameInstance) -> EngineTime
    public static native func GetTimeSystem(self: GameInstance) -> ref<TimeSystem>
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
