// Declarations of the natives provided by Cp2077Coop.dll (src/plugin). Keep in sync with
// src/plugin/CoopSystem.hpp and src/plugin/ScriptTypes.hpp.

public native struct CoopLocalSample {
    public native let valid: Bool;
    public native let position: Vector4;
    public native let yaw: Float;
    public native let pitch: Float;
    public native let locomotion: Int32;
    public native let flags: Int32;
}

public native struct CoopPuppetPose {
    public native let position: Vector4;
    public native let velocity: Vector4;
    public native let yaw: Float;
    public native let pitch: Float;
    public native let speed: Float;
    public native let locomotion: Int32;
    public native let flags: Int32;
    public native let rate: Float;
}

public native class CoopSystem extends IGameSystem {
    // Starts hosting on the given UDP port (0 = port from coop.ini) with an optional password.
    public native func Host(port: Int32, password: String) -> Bool
    // Joins "ip:port" with an optional password.
    public native func Join(address: String, password: String) -> Bool
    public native func Leave()
    public native func IsActive() -> Bool
    public native func IsHost() -> Bool
    public native func GetStatus() -> String
    // The plugin's version, e.g. "0.5.10-m1v".
    public native func GetVersion() -> String
    // Reads a value from red4ext/plugins/Cp2077Coop/coop.ini, e.g. "player.name".
    public native func GetSetting(key: String) -> String
    // Network impairment preset for testing: none, lan, good, typical, bad, awful.
    public native func SetImpairment(preset: String) -> Bool
    public native func SetDisplayName(name: String)
    public native func SetBridge(bridge: ref<IScriptable>)
    // Time fields (Sandevistan): start one for the local V, cancel it, and read this frame's rates.
    public native func ActivateTimeField(scale: Float, seconds: Float) -> Uint32
    public native func CancelTimeField(id: Uint32)
    public native func GetWorldRate() -> Float
    public native func GetLocalRate() -> Float
    public native func IsActivatingTimeField() -> Bool
    // Animation capture and direct-drive puppets (docs/07-testing-guide.md).
    public native func GetAnimStatus() -> String
    // Writes every animation input captured from your V to red4ext/plugins/Cp2077Coop/anim-record.txt; returns the path.
    public native func StartAnimRecording(seconds: Float) -> String
    // Lists the game's animation-related functions to red4ext/plugins/Cp2077Coop/anim-functions.txt.
    public native func DumpAnimFunctions() -> String
    // Writes what the animation graphs of this entity accept to red4ext/plugins/Cp2077Coop/anim-graphs-<label>.txt.
    public native func DumpAnimGraphs(entity: ref<IScriptable>, label: String) -> String
    // "events" (default) or "controller": how inputs are applied to puppets and the mirror.
    public native func SetAnimApplyVia(via: String)
    public native func GetAnimApplyVia() -> String
    // One test input: "name" sets a float input, "!name" pushes an event. Returns 1 if applied.
    public native func ApplyAnimTest(entity: ref<IScriptable>, name: String, value: Float) -> Int32
    // Applies your own V's animation inputs (and motion inputs) to this entity every frame; null stops.
    public native func MirrorAnimationsTo(entity: ref<IScriptable>)
    // Switches a puppet's AI controller on or off (and the classes in coop.ini [puppet] switchOff); false if the
    // game didn't let the plugin do it.
    public native func SetPuppetAI(entity: ref<IScriptable>, enabled: Bool) -> Bool
    // Comma-separated component classes switched along with the AI, e.g. "moveComponent".
    public native func SetSwitchOffComponents(list: String)
    public native func GetSwitchOffComponents() -> String
    // Puts a body at a position facing yaw (degrees): method 1 teleport, 2 transform, 3 AI teleport.
    public native func PlaceEntity(entity: ref<IScriptable>, position: Vector4, yaw: Float, method: Int32) -> Bool
    public native func GetPlacementError() -> String
    // coop.ini [puppet] place: 0 = auto, or a fixed method.
    public native func GetPlacementMethod() -> Int32
    public native func GetPlacementMethodName(method: Int32) -> String
    // Points a Codeware DynamicEntitySpec at an entity template by its path (instead of a TweakDB record). Not for
    // characters: spawned from a bare template they crash the game (round O); they need a Character record.
    public native func SetSpawnTemplate(spec: ref<IScriptable>, path: String) -> Bool
    // Switches every component of this class on or off; returns how many, -1 on error.
    public native func SetEntityComponents(entity: ref<IScriptable>, className: String, enabled: Bool) -> Int32
    // Called by Codeware (Entity/Initialize) for puppet and mirror bodies being built; not for scripts.
    public native func OnBodyInitialize(event: ref<IScriptable>)
    // Bodies spawned from now on: add an impostor to player bodies without one; lend V's gameplay animation sets
    // to NPC bodies.
    public native func SetBodyOptions(addImpostor: Bool, borrowAnimsets: Bool)
    // The current body options: 1 = add impostor, 2 = borrow animation sets.
    public native func GetBodyOptions() -> Int32
    // Your V's look on a body (CyberpunkMP's method): third-person flag, your items, your character customization.
    // Returns what was done.
    public native func ApplyMyLook(entity: ref<IScriptable>) -> String
    // Which of those steps run, for the mirror and for puppets from then on.
    public native func SetLookOptions(thirdPerson: Bool, items: Bool, customization: Bool)
    // 1 = third person, 2 = items, 4 = customization.
    public native func GetLookOptions() -> Int32
    // Whether puppets get their own player's look (coop.ini [look] apply).
    public native func SetPuppetLooks(on: Bool)
    public native func GetPuppetLooks() -> Bool
    // Graph input names for the motion values worked out from each puppet's pose; "" = not sent; "-name" negates.
    public native func SetMotionInputs(speed: String, direction: String, verticalSpeed: String, turnRate: String, moving: String)
    // The five names in use, comma-separated in that order.
    public native func GetMotionInputs() -> String
    // Whether the motion values also go out as V's graph's walking feature (playerLocomotion).
    public native func SetMotionFeature(on: Bool)
    public native func GetMotionFeature() -> Bool
    // Whether placed bodies are also told to animate as third person (TPPRepresentation feature).
    public native func SetTppFeature(on: Bool)
    public native func GetTppFeature() -> Bool
}

@addMethod(GameInstance)
public static native func GetCoopSystem() -> ref<CoopSystem>
