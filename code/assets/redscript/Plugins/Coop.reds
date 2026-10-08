// Seamless story co-op on top of CyberpunkMP.
//
// Everyone plays from their own story save. The first player to connect is the story host (the server decides,
// code/server/scripting/CoopSystem). The host's game reports its world every few seconds (time of day, weather,
// tracked quest), and the guests' games follow it: time only ever moves forward, the weather blends over. Chat
// commands starting with "/" go to the server (ChatController.SendChat): /help lists them.
//
// Engine calls that aren't known to compile on every patch go through CoopNative (C++, looked up by name at
// runtime), so a changed game function logs a warning instead of stopping all scripts from compiling.
module CyberpunkMP.Plugins

import CyberpunkMP.*

// C++: code/client/App/World/CoopNative.cpp.
public native class CoopNative extends IScriptable {
    public static native func GetGameTime() -> Int32;
    public static native func SetGameTime(seconds: Int32) -> Bool;
    public static native func GetWeather() -> CName;
    public static native func SetWeather(weather: CName) -> Bool;
    public static native func ResetWeather() -> Bool;
    public static native func GetTrackedQuest() -> String;
}

// Handled by the server (code/server/scripting/CoopSystem/ServerImpl.cs).
public native class CoopServer extends ServerRpc {
    // The story host's world: game time (seconds), weather, tracked quest.
    public static native func ReportWorld(gameTime: Int32, weather: CName, quest: String) -> Void;
    // A chat command, e.g. "/tp".
    public static native func Command(text: String) -> Void;
}

// Called by the server on this game. Runs without an instance: everything goes through CoopSession.
public class CoopClient extends ClientRpc {
    // Whether this game is the story host, and who is.
    public func SetRole(isHost: Bool, hostName: String) -> Void {
        CoopSession.Get().OnRole(isHost, hostName);
    }

    // The story host's world, for guests to follow.
    public func ApplyWorld(gameTime: Int32, weather: CName, quest: String) -> Void {
        CoopSession.Get().OnHostWorld(gameTime, weather, quest);
    }

    // Moves this V (e.g. /tp to the host).
    public func TeleportTo(x: Float, y: Float, z: Float) -> Void {
        CoopSession.Get().Teleport(x, y, z);
    }
}

public class CoopTickCallback extends DelayCallback {
    public let generation: Int32;

    public func Call() -> Void {
        CoopSession.Get().OnTick(this.generation);
    }
}

public class CoopSession extends ScriptableSystem {
    private let m_active: Bool;          // a role arrived from the server
    private let m_isHost: Bool;
    private let m_hostName: String;
    private let m_weather: CName;        // the host weather applied last (guests)
    private let m_weatherApplied: Bool;
    private let m_generation: Int32;     // one tick loop per connection

    public static func Get() -> ref<CoopSession> {
        return GameInstance.GetScriptableSystemsContainer(GetGameInstance()).Get(n"CyberpunkMP.Plugins.CoopSession") as CoopSession;
    }

    public func IsHost() -> Bool {
        return this.m_isHost;
    }

    public func OnRole(isHost: Bool, hostName: String) -> Void {
        let wasActive = this.m_active;
        this.m_active = true;
        this.m_isHost = isHost;
        this.m_hostName = hostName;
        FTLog(s"[Coop] story host: \(hostName)" + (isHost ? " (you)" : ""));
        if isHost && this.m_weatherApplied {
            // The weather is ours again.
            CoopNative.ResetWeather();
            this.m_weatherApplied = false;
            this.m_weather = n"None";
        }
        if isHost {
            this.Report();
        }
        if !wasActive {
            this.m_generation += 1;
            this.ScheduleTick();
        }
    }

    // Called by NetworkWorldSystem.OnDisconnected.
    public func OnDisconnected() -> Void {
        this.m_active = false;
        this.m_isHost = false;
        if this.m_weatherApplied {
            CoopNative.ResetWeather();
            this.m_weatherApplied = false;
            this.m_weather = n"None";
        }
    }

    public func OnTick(generation: Int32) -> Void {
        if !this.m_active || generation != this.m_generation {
            return;
        }
        if this.m_isHost {
            this.Report();
        }
        this.ScheduleTick();
    }

    public func OnHostWorld(gameTime: Int32, weather: CName, quest: String) -> Void {
        if this.m_isHost {
            return;
        }
        let mine = CoopNative.GetGameTime();
        if gameTime >= 0 && mine >= 0 {
            // How far the host's time of day is ahead of ours. Time only moves forward (going back confuses quest
            // timers), small drifts are left alone, and a guest just a little ahead doesn't skip a whole day.
            let day = 86400;
            let ahead = (gameTime % day) - (mine % day);
            if ahead < 0 {
                ahead += day;
            }
            if ahead > 120 && ahead < day - 600 {
                CoopNative.SetGameTime(mine + ahead);
            }
        }
        if NotEquals(weather, n"None") && NotEquals(weather, this.m_weather) {
            if CoopNative.SetWeather(weather) {
                this.m_weather = weather;
                this.m_weatherApplied = true;
            }
        }
    }

    public func Teleport(x: Float, y: Float, z: Float) -> Void {
        let player = GetPlayer(GetGameInstance());
        if !IsDefined(player) {
            return;
        }
        let rotation = Quaternion.ToEulerAngles(player.GetWorldOrientation());
        FTLog(s"[Coop] teleport to \(x), \(y), \(z)");
        let position: Vector4;
        position.X = x;
        position.Y = y;
        position.Z = z;
        position.W = 1.0;
        GameInstance.GetTeleportationFacility(GetGameInstance()).Teleport(player, position, rotation);
    }

    private func Report() -> Void {
        CoopServer.ReportWorld(CoopNative.GetGameTime(), CoopNative.GetWeather(), CoopNative.GetTrackedQuest());
    }

    private func ScheduleTick() -> Void {
        let callback = new CoopTickCallback();
        callback.generation = this.m_generation;
        GameInstance.GetDelaySystem(GetGameInstance()).DelayCallback(callback, 2.0, false);
    }
}

// Chat lines starting with "/" are co-op commands for the server, not messages.
public func CoopIsCommand(text: String) -> Bool {
    return Equals(UTF8StrLeft(text, 1), "/");
}
