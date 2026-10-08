// Script side of the co-op plugin: reports the local V to the plugin and shows remote players as puppets.
// The plugin calls these methods by name (src/plugin/RedGameAdapter.cpp), so keep names and signatures in sync.
//
// Only engine calls that were confirmed in game (spikes S1 and S3, docs/04-feasibility-and-risks.md §3) are
// used here, because a single unknown call would stop every script from compiling. Experiments stay in the CET
// dev panel (cet/coop-dev) until they are confirmed.
//
// Puppets (M0b):
// * Body: for now a plain NPC body, because it animates under AI move commands. The game's third-person V records
//   (Character.TPP_Player_Cutscene_Male/Female) look like V but only slide (round C lineup); they also copy the
//   LOCAL V's look and weapon, and only the one matching the local V's body spawns. They stay selectable in
//   coop.ini, with the record picked by the local V's body. Making a V body animate (S3c) and giving each puppet
//   its own player's looks (S1b) are open.
// * Time fields (Sandevistan, Kerenzikov): the session's world rate becomes the game's global time dilation, the
//   local V is exempt while activating, and a puppet whose player moves at a different rate than the local world
//   (e.g. the remote activator) gets its own rate that ignores the global one (spikes S8, S8b). coop.ini
//   [time] applyToGame=false turns this off.
// * Movement: AI move commands towards the network position (S3: normal walk/run animation). Teleports don't
//   move spawned NPCs, so a puppet that falls too far behind (fast travel, a jump off a roof) is respawned at
//   the right place instead.

public class CoopPuppetEntry {
    public let peer: Uint32;
    public let entityID: EntityID;
    public let female: Bool;       // body record used (the local V's body, see above)
    public let remoteFemale: Bool; // the remote player's own body, for when puppets can show it (S1b)
    public let spawnTime: Float;
    public let configured: Bool;
    // Walking (AI move commands).
    public let command: ref<AIMoveToCommand>;
    public let commandTarget: Vector4;
    public let commandTime: Float;
    public let commandType: Int32; // 0 walk, 1 run, 2 sprint
    public let commands: Int32;
    public let respawns: Int32;
    public let farSince: Float;    // when the puppet got too far from its target; -1 while it isn't
    public let noAI: Bool;         // the entity has no AI controller, so it can't walk
    // Personal time rate of the remote player (time fields), and the individual rate applied to the puppet
    // (0 = none, it follows the global time dilation).
    public let rate: Float;
    public let lastSeenRate: Float;
    public let appliedRate: Float;
    // Diagnostics for the CET panel.
    public let target: Vector4;
    public let actual: Vector4;
    public let speed: Float;
    public let found: Bool;
    public let foundTime: Float;   // when the current body was first seen
    public let lastRespawn: String; // why the puppet was last respawned
}

public class CoopBridge extends ScriptableSystem {
    private let m_puppets: array<ref<CoopPuppetEntry>>;
    private let m_ready: Bool;
    private let m_lastStatus: String;
    private let m_recordMale: TweakDBID;
    private let m_recordFemale: TweakDBID;
    private let m_recordMaleName: String;
    private let m_recordFemaleName: String;
    private let m_localFemale: Bool;
    // Time fields.
    private let m_applyTime: Bool;
    private let m_timeApplied: Bool;   // a global time dilation from the session is in effect
    private let m_appliedRate: Float;
    private let m_lastSeenRate: Float;
    private let m_ignoreApplied: Bool; // the local V is exempt from the global dilation

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
        this.LoadPuppetRecords(coop);
        this.m_applyTime = !this.IsOff(coop.GetSetting("time.applyToGame"));
        this.m_ready = true;
        coop.SetBridge(this);
    }

    private cb func OnSessionBeforeEnd(event: ref<GameSessionEvent>) {
        this.RemoveAllPuppets();
        this.ResetTime();
        this.m_ready = false;
        let coop = GameInstance.GetCoopSystem();
        if IsDefined(coop) {
            coop.SetBridge(null);
        }
    }

    // Records the puppets are spawned from: recordMale while the local V has a male body, recordFemale while it has a
    // female one. coop.ini can override them ([puppet] recordMale=..., recordFemale=...), e.g. to compare bodies
    // without rebuilding.
    private func LoadPuppetRecords(coop: ref<CoopSystem>) -> Void {
        // Default: a plain NPC body (r6/tweaks/Cp2077Coop/puppets.yaml). It walks, runs and sprints with real
        // animations under AI move commands, while the V-lookalikes only slide (round C lineup). To see the lookalike
        // instead: [puppet] recordMale=Character.TPP_Player_Cutscene_Male (recordFemale=..._Female) in coop.ini.
        this.m_recordMale = t"Cp2077Coop.Character.RemotePlayer";
        this.m_recordMaleName = "Cp2077Coop.Character.RemotePlayer";
        this.m_recordFemale = t"Cp2077Coop.Character.RemotePlayer";
        this.m_recordFemaleName = "Cp2077Coop.Character.RemotePlayer";
        let male = coop.GetSetting("puppet.recordMale");
        if StrLen(male) > 0 {
            this.m_recordMale = TDBID.Create(male);
            this.m_recordMaleName = male;
        }
        let female = coop.GetSetting("puppet.recordFemale");
        if StrLen(female) > 0 {
            this.m_recordFemale = TDBID.Create(female);
            this.m_recordFemaleName = female;
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
        sample.locomotion = 0; // later: crouch, sprint and so on from the player state machine
        sample.flags = 0;
        if this.LocalFemale() {
            sample.flags = 128; // body gender bit (kFemaleFlag in src/plugin/RedGameAdapter.cpp)
        }
        return sample;
    }

    public func DrivePuppet(peer: Uint32, pose: CoopPuppetPose) -> Void {
        if !this.m_ready {
            return;
        }

        let female = this.LocalFemale();
        let entry = this.FindPuppet(peer);
        if !IsDefined(entry) {
            this.SpawnPuppet(peer, pose, female);
            return;
        }

        entry.remoteFemale = (pose.flags & 128) != 0;
        entry.target = pose.position;
        entry.speed = pose.speed;
        entry.rate = pose.rate;
        let now = this.Now();

        if NotEquals(entry.female, female) {
            entry.lastRespawn = "your V's body changed";
            this.Respawn(entry, pose, female);
            return;
        }

        let entity = GameInstance.GetDynamicEntitySystem().GetEntity(entry.entityID);
        if !IsDefined(entity) {
            entry.found = false;
            // A V body can take a while to stream in the first time, so give it plenty of time before trying again.
            if now - entry.spawnTime > 30.0 {
                entry.lastRespawn = "didn't appear within 30 s";
                this.Respawn(entry, pose, female);
            }
            return;
        }
        if !entry.found {
            entry.found = true;
            entry.foundTime = now;
        }
        entry.actual = entity.GetWorldPosition();

        let puppet = entity as ScriptedPuppet;
        if !IsDefined(puppet) {
            entry.noAI = true;
            return;
        }
        if !entry.configured {
            // Friendly to the local V: no hostility when bumped, and V can share a car with it (S1 vehicles).
            puppet.GetAttitudeAgent().SetAttitudeGroup(n"player");
            entry.configured = true;
        }
        this.ApplyPuppetRate(entry, puppet);

        // Too far to walk: respawn at the target. A body that has just appeared can report a wrong position for a
        // moment (round C: puppets were respawned in a loop the moment they appeared), so this only applies once the
        // body has been there for 3 s, and at most every 10 s.
        let offBy = Vector4.Distance(entry.actual, pose.position);
        if offBy > 8.0 {
            if entry.farSince < 0.0 {
                entry.farSince = now;
            }
        } else {
            entry.farSince = -1.0;
        }
        let settled = now - entry.foundTime > 3.0 && now - entry.spawnTime > 10.0;
        if settled && (offBy > 40.0 || (entry.farSince >= 0.0 && now - entry.farSince > 3.0)) {
            entry.lastRespawn = s"\(FloatToStringPrec(offBy, 0)) m from where it should be";
            this.Respawn(entry, pose, female);
            return;
        }

        this.Walk(entry, puppet, pose, offBy, now);
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

    // Called every frame during a session with the session's world rate, and once with 1.0 when a session ends.
    // The engine is only called when something changes: the rate is applied once it has settled (the session eases
    // between rates over 300 ms) and moved by more than 0.02, and V's exemption only when it flips.
    public func ApplyTimeRates(world: Float, activating: Bool) -> Void {
        if !this.m_ready || !this.m_applyTime {
            return;
        }
        let settled = AbsF(world - this.m_lastSeenRate) < 0.001;
        this.m_lastSeenRate = world;
        let timeSystem = GameInstance.GetTimeSystem(this.GetGameInstance());

        if world < 0.99 {
            if !settled {
                return;
            }
            if !this.m_timeApplied || AbsF(world - this.m_appliedRate) > 0.02 {
                timeSystem.SetTimeDilation(n"coopField", world, 999.0);
                this.m_appliedRate = world;
                this.m_timeApplied = true;
            }
            if NotEquals(activating, this.m_ignoreApplied) {
                timeSystem.SetIgnoreTimeDilationOnLocalPlayerZero(activating);
                this.m_ignoreApplied = activating;
            }
            return;
        }
        this.ResetTime();
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

    // Puppet by index (0 .. GetPuppetCount() - 1), and the remote player's personal time rate. Used by the
    // panel's experimental time-field switch.
    public func GetPuppetEntityAt(index: Int32) -> ref<Entity> {
        if index < 0 || index >= ArraySize(this.m_puppets) {
            return null;
        }
        return GameInstance.GetDynamicEntitySystem().GetEntity(this.m_puppets[index].entityID);
    }

    public func GetPuppetIdAt(index: Int32) -> EntityID {
        let none: EntityID;
        if index < 0 || index >= ArraySize(this.m_puppets) {
            return none;
        }
        return this.m_puppets[index].entityID;
    }

    public func GetPuppetRateAt(index: Int32) -> Float {
        if index < 0 || index >= ArraySize(this.m_puppets) {
            return 1.0;
        }
        return this.m_puppets[index].rate;
    }

    // One line per puppet for the CET panel: where the network says it should be, where it is, and how it
    // got there.
    public func GetPuppetDebug() -> String {
        let record = this.LocalFemale() ? this.m_recordFemaleName : this.m_recordMaleName;
        let text = s"body: \(record)\n";
        if !this.m_applyTime {
            text = text + "time fields: not applied to the game (coop.ini [time] applyToGame)\n";
        } else {
            if this.m_timeApplied {
                text = text + s"time fields: world slowed to x\(FloatToStringPrec(this.m_appliedRate, 2))";
                if this.m_ignoreApplied {
                    text = text + ", you exempt";
                }
                text = text + "\n";
            }
        }
        let now = this.Now();
        for entry in this.m_puppets {
            let theirs = entry.remoteFemale ? "female" : "male";
            let line = s"player \(entry.peer) (their V: \(theirs)): ";
            if !entry.found {
                let body = entry.female ? "female" : "male";
                line = line + s"\(body) body not there yet, waiting \(FloatToStringPrec(now - entry.spawnTime, 0)) s";
                line = line + s" (tries again after 30 s), respawns \(entry.respawns)";
                if StrLen(entry.lastRespawn) > 0 {
                    line = line + s", last respawn: \(entry.lastRespawn)";
                }
            } else {
                let offBy = Vector4.Distance(entry.target, entry.actual);
                line = line + s"target \(this.FormatXY(entry.target)), actual \(this.FormatXY(entry.actual)), ";
                line = line + s"off by \(FloatToStringPrec(offBy, 1)) m, speed \(FloatToStringPrec(entry.speed, 1)) m/s, ";
                line = line + s"move commands \(entry.commands), respawns \(entry.respawns)";
                if entry.appliedRate > 0.0 {
                    line = line + s", own time rate x\(FloatToStringPrec(entry.appliedRate, 2))";
                }
                if StrLen(entry.lastRespawn) > 0 {
                    line = line + s" (last: \(entry.lastRespawn))";
                }
                if entry.noAI {
                    line = line + ", NO AI (can't walk)";
                }
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

    private func IsOff(value: String) -> Bool {
        return Equals(value, "false") || Equals(value, "0") || Equals(value, "no") || Equals(value, "off");
    }

    // Back to normal time: the session's global dilation and V's exemption are removed. Puppets lose their own rates
    // in ApplyPuppetRate.
    private func ResetTime() -> Void {
        if !this.m_timeApplied && !this.m_ignoreApplied {
            return;
        }
        let timeSystem = GameInstance.GetTimeSystem(this.GetGameInstance());
        if this.m_timeApplied {
            timeSystem.UnsetTimeDilation(n"coopField");
        }
        if this.m_ignoreApplied {
            timeSystem.SetIgnoreTimeDilationOnLocalPlayerZero(false);
        }
        this.m_timeApplied = false;
        this.m_ignoreApplied = false;
        this.m_appliedRate = 1.0;
    }

    // A puppet whose player runs at a different rate than the local world gets that rate as its own, ignoring the
    // global dilation: the remote activator stays at full speed in a slowed world. Applied once the rate has settled.
    private func ApplyPuppetRate(entry: ref<CoopPuppetEntry>, puppet: ref<ScriptedPuppet>) -> Void {
        if !this.m_applyTime {
            return;
        }
        let settled = AbsF(entry.rate - entry.lastSeenRate) < 0.001;
        entry.lastSeenRate = entry.rate;
        if !settled {
            return;
        }
        let world = 1.0;
        if this.m_timeApplied {
            world = this.m_appliedRate;
        }
        if AbsF(entry.rate - world) > 0.02 {
            if AbsF(entry.rate - entry.appliedRate) > 0.02 {
                puppet.SetIndividualTimeDilation(n"coopRate", entry.rate, 999.0, n"None", n"None", true);
                entry.appliedRate = entry.rate;
            }
        } else {
            if entry.appliedRate > 0.0 {
                puppet.UnsetIndividualTimeDilation();
                entry.appliedRate = 0.0;
            }
        }
    }

    // The local V's body; remembers the last answer while there is no V (loading screens).
    private func LocalFemale() -> Bool {
        let player = GetPlayer(this.GetGameInstance());
        if IsDefined(player) {
            this.m_localFemale = Equals(player.GetResolvedGenderName(), n"Female");
        }
        return this.m_localFemale;
    }

    private func Now() -> Float {
        return EngineTime.ToFloat(GameInstance.GetSimTime(this.GetGameInstance()));
    }

    private func FindPuppet(peer: Uint32) -> ref<CoopPuppetEntry> {
        for entry in this.m_puppets {
            if entry.peer == peer {
                return entry;
            }
        }
        return null;
    }

    private func SpawnPuppet(peer: Uint32, pose: CoopPuppetPose, female: Bool) -> Void {
        let entry = new CoopPuppetEntry();
        entry.peer = peer;
        entry.female = female;
        entry.farSince = -1.0;
        entry.rate = 1.0;
        entry.entityID = this.CreatePuppetEntity(pose, female);
        entry.spawnTime = this.Now();
        ArrayPush(this.m_puppets, entry);
    }

    private func Respawn(entry: ref<CoopPuppetEntry>, pose: CoopPuppetPose, female: Bool) -> Void {
        GameInstance.GetDynamicEntitySystem().DeleteEntity(entry.entityID);
        entry.entityID = this.CreatePuppetEntity(pose, female);
        entry.female = female;
        entry.spawnTime = this.Now();
        entry.configured = false;
        entry.command = null;
        entry.farSince = -1.0;
        entry.found = false;
        entry.noAI = false;
        entry.appliedRate = 0.0;
        entry.respawns += 1;
    }

    private func CreatePuppetEntity(pose: CoopPuppetPose, female: Bool) -> EntityID {
        let rotation: EulerAngles;
        rotation.Yaw = pose.yaw;

        let spec = new DynamicEntitySpec();
        spec.recordID = female ? this.m_recordFemale : this.m_recordMale;
        spec.position = pose.position;
        spec.orientation = EulerAngles.ToQuat(rotation);
        // Same settings as the CET probe that spawns lookalikes successfully (round C). The earlier spawnInView,
        // persistState and persistSpawn settings are left at their defaults.
        spec.alwaysSpawned = true;
        spec.tags = [n"Cp2077Coop.Puppet"];
        return GameInstance.GetDynamicEntitySystem().CreateEntity(spec);
    }

    // Steers the puppet with AI move commands. The target is the network position plus a short lead along the
    // remote player's velocity, so a walking puppet keeps walking between updates instead of stopping at
    // every target. Commands are re-sent at most five times a second.
    private func Walk(entry: ref<CoopPuppetEntry>, puppet: ref<ScriptedPuppet>, pose: CoopPuppetPose, offBy: Float, now: Float) -> Void {
        let moving = pose.speed > 0.3;
        if !moving && offBy < 0.6 {
            return; // standing: let the last command finish where it is
        }

        // Lead 0.5 s along the velocity: covers the network buffer (~0.1 s) plus the AI's own reaction.
        let lead = pose.position;
        if moving {
            lead = pose.position + pose.velocity * 0.5;
        }
        let type = this.MovementType(pose.speed, offBy);
        let sinceLast = now - entry.commandTime;
        let moved = Vector4.Distance(lead, entry.commandTarget);
        let send = !IsDefined(entry.command)
            || (sinceLast > 0.2 && (type != entry.commandType || moved > 0.75))
            || (sinceLast > 1.0 && offBy > 0.6);
        if !send {
            return;
        }

        let ai = puppet.GetAIControllerComponent();
        if !IsDefined(ai) {
            entry.noAI = true;
            return;
        }
        if IsDefined(entry.command) {
            ai.CancelCommand(entry.command);
        }

        let destination: WorldPosition;
        WorldPosition.SetVector4(destination, lead);
        let spec: AIPositionSpec;
        AIPositionSpec.SetWorldPosition(spec, destination);

        let command = new AIMoveToCommand();
        command.movementTarget = spec;
        command.movementType = this.ToMovementType(type);
        command.ignoreNavigation = true;
        command.desiredDistanceFromTarget = 0.3;
        command.finishWhenDestinationReached = true;
        ai.SendCommand(command);

        entry.command = command;
        entry.commandTarget = lead;
        entry.commandTime = now;
        entry.commandType = type;
        entry.commands += 1;
    }

    // Walk below 2 m/s, run below 5 m/s, sprint above; one step faster when more than 3 m behind, and sprint when
    // more than 6 m behind.
    private func MovementType(speed: Float, offBy: Float) -> Int32 {
        let type = 0;
        if speed > 5.0 {
            type = 2;
        } else {
            if speed > 2.0 {
                type = 1;
            }
        }
        if offBy > 3.0 && type < 2 {
            type += 1;
        }
        if offBy > 6.0 {
            type = 2;
        }
        return type;
    }

    private func ToMovementType(type: Int32) -> moveMovementType {
        if type >= 2 {
            return moveMovementType.Sprint;
        }
        if type == 1 {
            return moveMovementType.Run;
        }
        return moveMovementType.Walk;
    }
}
