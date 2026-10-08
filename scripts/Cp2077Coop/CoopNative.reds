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
}

@addMethod(GameInstance)
public static native func GetCoopSystem() -> ref<CoopSystem>
