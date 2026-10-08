// Script side of the co-op plugin (M0a): reports the local V to the plugin and shows remote players as
// puppets. The plugin calls these methods by name (src/plugin/RedGameAdapter.cpp), so keep names and
// signatures in sync.
//
// This file deliberately uses only engine calls that are widely used by existing mods, because a single
// unknown call would stop every script from compiling. Experimental engine calls (AI-driven walking,
// attitude changes, gender detection) live in the CET dev panel (cet/coop-dev) until spikes S1-S3
// confirm them.

public class CoopPuppetEntry {
    public let peer: Uint32;
    public let entityID: EntityID;
    // Teleport bookkeeping (M0a moves puppets by teleporting; animated walking is M0b).
    public let lastSent: Vector4;
    public let lastSentYaw: Float;
    public let lastTeleportTime: Float;
    public let teleports: Int32;
    // Diagnostics for the CET panel.
    public let target: Vector4;
    public let actual: Vector4;
    public let found: Bool;
}

public class CoopBridge extends ScriptableSystem {
    private let m_puppets: array<ref<CoopPuppetEntry>>;
    private let m_ready: Bool;
    private let m_lastStatus: String;

    // --- lifecycle -----------------------------------------------------------------------------------

    private func OnAttach() -> Void {
        let callbacks = GameInstance.GetCallbackSystem();
        callbacks.RegisterCallback(n"Session/Ready", this, n"OnSessionReady");
        callbacks.RegisterCallback(n"Session/BeforeEnd", this, n"OnSessionBeforeEnd");
    }

    private cb func OnSessionReady(event: ref<GameSessionEvent>) {
        if event.IsPreGame() {
            return; // main menu
        }
        let coop = GameInstance.GetCoopSystem();
        if !IsDefined(coop) {
            return; // the plugin is not installed or failed to load
        }
        this.m_ready = true;
        coop.SetBridge(this);
    }

    private cb func OnSessionBeforeEnd(event: ref<GameSessionEvent>) {
        this.RemoveAllPuppets();
        this.m_ready = false;
        let coop = GameInstance.GetCoopSystem();
        if IsDefined(coop) {
            coop.SetBridge(null);
        }
    }

    // --- called by the plugin ------------------------------------------------------------------------

    public func CaptureLocal() -> CoopLocalSample {
        let sample: CoopLocalSample;
        let player = GetPlayer(this.GetGameInstance());
        if !this.m_ready || !IsDefined(player) {
            sample.valid = false;
            return sample;
        }

        sample.valid = true;
        sample.position = player.GetWorldPosition();
        let rotation = Vector4.ToRotation(player.GetWorldForward());
        sample.yaw = rotation.Yaw;
        sample.pitch = 0.0;
        sample.locomotion = 0; // M0b: player state-machine locomotion (spike S3)
        sample.flags = 0;      // M0b: body gender (spike S1)
        return sample;
    }

    public func DrivePuppet(peer: Uint32, pose: CoopPuppetPose) -> Void {
        if !this.m_ready {
            return;
        }

        let entry = this.FindPuppet(peer);
        if !IsDefined(entry) {
            this.SpawnPuppet(peer, pose);
            return;
        }

        entry.target = pose.position;
        let puppet = GameInstance.GetDynamicEntitySystem().GetEntity(entry.entityID) as GameObject;
        if !IsDefined(puppet) {
            entry.found = false;
            return; // still spawning or streamed out
        }
        entry.found = true;
        entry.actual = puppet.GetWorldPosition();

        // M0a moves puppets by teleporting; animated walking comes in M0b.
        // A teleport may take more than one frame to apply, and a new request can cancel the pending one,
        // so only send the next teleport once the previous one has landed (or after a timeout).
        let now = EngineTime.ToFloat(GameInstance.GetSimTime(this.GetGameInstance()));
        let offTarget = Vector4.Distance(entry.actual, pose.position) > 0.2
            || AbsF(pose.yaw - entry.lastSentYaw) > 10.0;
        let landed = entry.teleports == 0 || Vector4.Distance(entry.actual, entry.lastSent) < 0.5;
        let timedOut = now - entry.lastTeleportTime > 0.5;
        if !offTarget || !(landed || timedOut) {
            return;
        }

        let rotation: EulerAngles;
        rotation.Yaw = pose.yaw;
        GameInstance.GetTeleportationFacility(this.GetGameInstance()).Teleport(puppet, pose.position, rotation);
        entry.lastSent = pose.position;
        entry.lastSentYaw = pose.yaw;
        entry.lastTeleportTime = now;
        entry.teleports += 1;
    }

    public func RemovePuppet(peer: Uint32) -> Void {
        let i = 0;
        while i < ArraySize(this.m_puppets) {
            if this.m_puppets[i].peer == peer {
                GameInstance.GetDynamicEntitySystem().DeleteEntity(this.m_puppets[i].entityID);
                ArrayErase(this.m_puppets, i);
                return;
            }
            i += 1;
        }
    }

    public func RemoveAllPuppets() -> Void {
        for entry in this.m_puppets {
            GameInstance.GetDynamicEntitySystem().DeleteEntity(entry.entityID);
        }
        ArrayClear(this.m_puppets);
    }

    public func OnPlayerJoined(peer: Uint32, name: String) -> Void {
        this.ShowStatus(s"\(name) joined");
    }

    public func ShowStatus(text: String) -> Void {
        this.m_lastStatus = text;
        FTLog(s"[Co-op] \(text)");
    }

    // --- helpers for the CET dev panel ---------------------------------------------------------------
    // Pass-throughs to CoopSystem, in case CET cannot reach GameInstance.GetCoopSystem() directly.

    public func DevHost(port: Int32, password: String) -> Bool {
        return GameInstance.GetCoopSystem().Host(port, password);
    }

    public func DevJoin(address: String, password: String) -> Bool {
        return GameInstance.GetCoopSystem().Join(address, password);
    }

    public func DevLeave() -> Void {
        GameInstance.GetCoopSystem().Leave();
    }

    public func DevStatus() -> String {
        return GameInstance.GetCoopSystem().GetStatus();
    }

    public func DevImpairment(preset: String) -> Bool {
        return GameInstance.GetCoopSystem().SetImpairment(preset);
    }

    public func GetLastStatus() -> String {
        return this.m_lastStatus;
    }

    public func GetPuppetCount() -> Int32 {
        return ArraySize(this.m_puppets);
    }

    // One line per puppet: where the network says it should be, where it actually is, and how many
    // teleports were sent. Shown in the CET panel.
    public func GetPuppetDebug() -> String {
        let text = "";
        for entry in this.m_puppets {
            let line = s"player \(entry.peer): ";
            if !entry.found {
                line = line + "entity not found (spawning or streamed out)";
            } else {
                let offBy = Vector4.Distance(entry.target, entry.actual);
                line = line + s"target \(this.FormatXY(entry.target)), actual \(this.FormatXY(entry.actual)), ";
                line = line + s"off by \(FloatToStringPrec(offBy, 1)) m, teleports \(entry.teleports)";
            }
            text = text + line + "\n";
        }
        return text;
    }

    private func FormatXY(v: Vector4) -> String {
        return s"(\(FloatToStringPrec(v.X, 1)), \(FloatToStringPrec(v.Y, 1)))";
    }

    public func GetPuppetEntity(peer: Uint32) -> ref<Entity> {
        let entry = this.FindPuppet(peer);
        if !IsDefined(entry) {
            return null;
        }
        return GameInstance.GetDynamicEntitySystem().GetEntity(entry.entityID);
    }

    // --- internals -------------------------------------------------------------------------------------

    private func FindPuppet(peer: Uint32) -> ref<CoopPuppetEntry> {
        for entry in this.m_puppets {
            if entry.peer == peer {
                return entry;
            }
        }
        return null;
    }

    private func SpawnPuppet(peer: Uint32, pose: CoopPuppetPose) -> Void {
        let rotation: EulerAngles;
        rotation.Yaw = pose.yaw;

        let spec = new DynamicEntitySpec();
        // The record is defined in r6/tweaks/Cp2077Coop/puppets.yaml; spike S1 picks its base.
        spec.recordID = t"Cp2077Coop.Character.RemotePlayer";
        spec.position = pose.position;
        spec.orientation = EulerAngles.ToQuat(rotation);
        spec.alwaysSpawned = true;
        spec.spawnInView = true;
        spec.persistState = false;
        spec.persistSpawn = false;
        spec.tags = [n"Cp2077Coop.Puppet"];

        let entry = new CoopPuppetEntry();
        entry.peer = peer;
        entry.entityID = GameInstance.GetDynamicEntitySystem().CreateEntity(spec);
        ArrayPush(this.m_puppets, entry);
    }
}
