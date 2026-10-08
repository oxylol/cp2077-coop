// Script side of the co-op plugin: reports the local V to the plugin and shows remote players as puppets.
// The plugin calls these methods by name (src/plugin/RedGameAdapter.cpp), so keep names and signatures in sync.
//
// Only engine calls that were confirmed in game (spikes S1, S3 and S8, docs/04-feasibility-and-risks.md §3) are
// used here, because a single unknown call would stop every script from compiling. Calls that are not confirmed
// yet go through the plugin (CoopSystem.SetPuppetAI), which logs an error instead. Experiments stay in the CET dev
// panel (cet/coop-dev) until they are confirmed.
//
// Puppets ("direct drive", docs/01-architecture.md §4):
// * Body (0.6): the player's own third-person body (Cp2077Coop.Character.PlayerBody_Male/Female,
//   tweaks/Cp2077Coop/bodies.tweak), of the remote player's gender once their look has arrived. It has V's own
//   animation graph, so the animation inputs the plugin captures from the remote player's V can be applied to it
//   unchanged (src/plugin/AnimCapture.cpp). The plugin gives it the remote player's look when it appears
//   (CyberpunkMP's method, src/plugin/Looks.cpp): their items, their character customization, third person. A
//   player without a look of their own (a coop-sim bot) gets a body of the local V's gender and the local V's
//   look. coop.ini [look] apply=false goes back to 0.5's body: the cutscene lookalike of the LOCAL V's gender
//   (Character.TPP_Player_Cutscene_Male/Female), which copies the local V through its impostor (S1c).
// * Movement: no AI routing. The body is placed every frame at the network position, facing the network yaw (the
//   session interpolates the pose, ~0.1 s behind), through the plugin (CoopSystem.PlaceEntity). Round F showed the
//   teleportation facility doesn't move NPCs, AI on or off, so placement methods are tried in turn: Codeware's
//   SetWorldTransform with the AI off, then an AI teleport command with the AI on, then the teleportation facility
//   (coop.ini [puppet] place). Speed,
//   direction, turning and so on reach its animation graph as inputs: captured from the remote V, plus motion
//   values the plugin works out from the pose (core/AnimMotion.hpp, coop.ini [anim]).
// * Fallback: placing an NPC with its AI on doesn't move it (S3), and placing one with its AI off is new. If the
//   body stays put while the placements move away from it, that puppet goes back to AI walking (AI move commands
//   towards the network position, the M0b behaviour) and the dev panel says why; it tries direct drive again a
//   minute later, or with its next body, and stays on AI walking after the second fallback. coop.ini [puppet]
//   drive=ai uses AI walking from the start.
// * Time fields (Sandevistan, Kerenzikov): the session's world rate becomes the game's global time dilation, the
//   local V is exempt while activating, and a puppet whose player moves at a different rate than the local world
//   (e.g. the remote activator) gets its own rate that ignores the global one (spikes S8, S8b). coop.ini
//   [time] applyToGame=false turns this off.

public class CoopPuppetEntry {
    public let peer: Uint32;
    public let entityID: EntityID;
    public let female: Bool;       // body record used (see PuppetFemale)
    public let remoteFemale: Bool; // the remote player's own body
    public let ownLook: Bool;      // the remote player sent their look (the plugin puts it on the body)
    public let spawnTime: Float;
    public let configured: Bool;
    // Direct drive: AI off, placed every frame. False = AI walking (coop.ini, or the fallback below).
    public let direct: Bool;
    public let method: Int32;        // placement method (CoopSystem.PlaceEntity: 1 teleport, 2 transform, 3 AI teleport)
    public let methodNote: String;   // why the previous method was given up
    public let lastPlaceTime: Float;
    public let aiOff: Bool;          // the plugin switched the AI off
    public let aiNote: String;       // what happened when switching it
    public let placements: Int32;
    public let placedAt: Vector4;    // where the last placement put it
    public let placedYaw: Float;
    public let stuckSince: Float;    // when the body stopped following the placements; -1 while it follows
    public let lastActual: Vector4;  // where the body was the frame before
    public let aiTries: Int32;       // attempts to switch the AI off (the controller may not be ready at once)
    public let aiTryTime: Float;
    public let fallback: String;     // why this puppet last went back to AI walking
    public let fallbacks: Int32;     // after two, the puppet stays on AI walking
    public let fallbackTime: Float;
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
    private let m_applyLooks: Bool;  // coop.ini [look] apply (default on): puppets get their own player's look
    private let m_driveDirect: Bool;
    private let m_placeMethod: Int32; // coop.ini [puppet] place: 0 = auto (transform, then AI teleport)
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

    // Records the puppets are spawned from: recordMale for a male body, recordFemale for a female one (see
    // PuppetFemale), and how they are moved. coop.ini can override them ([puppet] recordMale=..., recordFemale=...,
    // drive=direct|ai), e.g. to compare bodies without rebuilding.
    private func LoadPuppetRecords(coop: ref<CoopSystem>) -> Void {
        // Default: the player's own third-person body, which has V's animation graph (the inputs captured from the
        // remote V fit it) and gets its player's look from the plugin. Without looks: the cutscene lookalike, which
        // copies the local V. The plain NPC body of M0b is Cp2077Coop.Character.RemotePlayer (r6/tweaks/Cp2077Coop).
        this.m_applyLooks = !this.IsOff(coop.GetSetting("look.apply"));
        if this.m_applyLooks {
            this.m_recordMale = t"Cp2077Coop.Character.PlayerBody_Male";
            this.m_recordMaleName = "Cp2077Coop.Character.PlayerBody_Male";
            this.m_recordFemale = t"Cp2077Coop.Character.PlayerBody_Female";
            this.m_recordFemaleName = "Cp2077Coop.Character.PlayerBody_Female";
        } else {
            this.m_recordMale = t"Character.TPP_Player_Cutscene_Male";
            this.m_recordMaleName = "Character.TPP_Player_Cutscene_Male";
            this.m_recordFemale = t"Character.TPP_Player_Cutscene_Female";
            this.m_recordFemaleName = "Character.TPP_Player_Cutscene_Female";
        }
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
        let drive = coop.GetSetting("puppet.drive");
        this.m_driveDirect = !Equals(drive, "ai") && !Equals(drive, "AI");
        this.m_placeMethod = coop.GetPlacementMethod();
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

        let female = this.PuppetFemale(pose);
        let entry = this.FindPuppet(peer);
        if !IsDefined(entry) {
            this.SpawnPuppet(peer, pose, female);
            return;
        }

        entry.remoteFemale = (pose.flags & 128) != 0;
        entry.ownLook = (pose.flags & 256) != 0;
        entry.target = pose.position;
        entry.speed = pose.speed;
        entry.rate = pose.rate;
        let now = this.Now();

        if NotEquals(entry.female, female) {
            entry.lastRespawn = entry.ownLook ? "their V's body arrived" : "your V's body changed";
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
        let position = entity.GetWorldPosition();
        if !entry.found {
            entry.found = true;
            entry.foundTime = now;
            entry.actual = position;
        }
        entry.lastActual = entry.actual;
        entry.actual = position;

        let object = entity as GameObject;
        if !IsDefined(object) {
            entry.noAI = true;
            return;
        }
        if !entry.configured {
            // Friendly to the local V: no hostility when bumped, and V can share a car with it (S1 vehicles).
            object.GetAttitudeAgent().SetAttitudeGroup(n"player");
            entry.configured = true;
        }
        // Individual time dilation lives on puppets (TimeDilatable), not on every GameObject.
        let puppet = entity as ScriptedPuppet;
        if IsDefined(puppet) {
            this.ApplyPuppetRate(entry, puppet);
        }

        // A puppet that fell back to AI walking tries direct drive again after a minute (the cause may have been
        // passing, e.g. a load), at most twice per puppet.
        if !entry.direct && this.m_driveDirect && entry.fallbacks > 0 && entry.fallbacks < 2 && now - entry.fallbackTime > 60.0 {
            this.RetryDirect(entry);
        }

        if entry.direct {
            if this.NeedsAI(entry.method) {
                if entry.aiOff {
                    this.SwitchAI(entry, entity, true);
                }
            } else {
                // Switch the AI off; retried every 0.5 s for 3 s in case the controller isn't ready at first.
                if !entry.aiOff && entry.aiTries < 6 && now - entry.aiTryTime > 0.5 {
                    entry.aiTries += 1;
                    entry.aiTryTime = now;
                    this.SwitchAI(entry, entity, false);
                }
            }
            this.Place(entry, entity, pose, female, now);
            return;
        }

        // AI walking. Too far to walk: respawn at the target. A body that has just appeared can report a wrong
        // position for a moment (round C: puppets were respawned in a loop the moment they appeared), so this only
        // applies once the body has been there for 3 s, and at most every 10 s.
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

        if !IsDefined(puppet) {
            entry.noAI = true;
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
        let text = "";
        if this.m_applyLooks {
            text = s"bodies: \(this.m_recordMaleName) / \(this.m_recordFemaleName) (by each player's V), with their look\n";
        } else {
            let record = this.LocalFemale() ? this.m_recordFemaleName : this.m_recordMaleName;
            text = s"body: \(record) (picked by your V's body; looks off)\n";
        }
        if this.m_driveDirect {
            let place = GameInstance.GetCoopSystem().GetPlacementMethodName(this.m_placeMethod);
            text = text + s"movement: direct, placed every frame (method \(place); falls back to AI walking if the body doesn't follow)\n";
        } else {
            text = text + "movement: AI walking (coop.ini [puppet] drive=ai)\n";
        }
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
            let look = entry.ownLook ? "their look" : "your look";
            let line = s"player \(entry.peer) (their V: \(theirs), \(look)): ";
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
                line = line + s"off by \(FloatToStringPrec(offBy, 2)) m, speed \(FloatToStringPrec(entry.speed, 1)) m/s, ";
                if entry.direct {
                    let method = GameInstance.GetCoopSystem().GetPlacementMethodName(entry.method);
                    line = line + s"DIRECT (\(method)): placed \(entry.placements) times";
                    if StrLen(entry.methodNote) > 0 {
                        line = line + s" (\(entry.methodNote))";
                    }
                    if this.NeedsAI(entry.method) {
                        line = line + ", AI on (this method needs it)";
                    } else {
                        if entry.aiOff {
                            line = line + ", AI off";
                        } else {
                            line = line + s", AI NOT off after \(entry.aiTries) tries (\(entry.aiNote))";
                        }
                    }
                    if entry.fallbacks > 0 {
                        line = line + s", earlier fallbacks \(entry.fallbacks)";
                    }
                    if entry.stuckSince >= 0.0 {
                        line = line + s", NOT FOLLOWING for \(FloatToStringPrec(now - entry.stuckSince, 1)) s";
                    }
                } else {
                    line = line + s"AI WALKING: move commands \(entry.commands)";
                    if StrLen(entry.fallback) > 0 {
                        line = line + s" (fallback \(entry.fallbacks): \(entry.fallback))";
                    }
                }
                line = line + s", respawns \(entry.respawns)";
                if entry.appliedRate > 0.0 {
                    line = line + s", own time rate x\(FloatToStringPrec(entry.appliedRate, 2))";
                }
                if StrLen(entry.lastRespawn) > 0 {
                    line = line + s" (last respawn: \(entry.lastRespawn))";
                }
                if entry.noAI {
                    line = line + ", NOT A GAME OBJECT OR NO AI (can't be moved)";
                }
            }
            text = text + line + "\n";
        }
        return text;
    }

    private func FormatXY(v: Vector4) -> String {
        return s"(\(FloatToStringPrec(v.X, 1)), \(FloatToStringPrec(v.Y, 1)))";
    }

    // Whether a puppet is placed every frame (the plugin feeds motion inputs only to those).
    public func IsPuppetDirect(peer: Uint32) -> Bool {
        let entry = this.FindPuppet(peer);
        if !IsDefined(entry) {
            return false;
        }
        return entry.direct;
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

    // The body a puppet gets: its own player's (bit 128 of the pose flags) when that player sent their look (bit
    // 256) and looks are on; otherwise the local V's, which is the look it is given (impostor, or the plugin's
    // fallback for players without a look).
    private func PuppetFemale(pose: CoopPuppetPose) -> Bool {
        if this.m_applyLooks && (pose.flags & 256) != 0 {
            return (pose.flags & 128) != 0;
        }
        return this.LocalFemale();
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
        entry.stuckSince = -1.0;
        entry.rate = 1.0;
        entry.direct = this.m_driveDirect;
        entry.method = this.FirstMethod();
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
        entry.aiOff = false;
        entry.aiNote = "";
        entry.aiTries = 0;
        entry.aiTryTime = 0.0;
        entry.placements = 0;
        entry.stuckSince = -1.0;
        entry.respawns += 1;
        // A new body gets direct drive again, unless it already failed twice for this player.
        entry.lastPlaceTime = 0.0;
        if this.m_driveDirect && entry.fallbacks < 2 {
            entry.direct = true;
            entry.method = this.FirstMethod();
            entry.methodNote = "";
        }
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

    // --- direct drive ----------------------------------------------------------------------------------

    // Puts the body at the network pose. Called every frame; the engine is only called when the pose moved or
    // turned, or the body drifted off it.
    //
    // A body that doesn't take placements stays where it is while the placements move away from it. That is what
    // counts as stuck: more than 1 m (horizontally) from the last placement while the body itself didn't move since
    // the frame before. A body that follows a frame or two late, or drops a little onto the ground, keeps moving and
    // never counts. Stuck for 2 s: the puppet falls back to AI walking, or, if it has no AI to walk with, is
    // respawned at the right place.
    private func Place(entry: ref<CoopPuppetEntry>, entity: ref<Entity>, pose: CoopPuppetPose, female: Bool, now: Float) -> Void {
        if entry.placements > 0 {
            let lagSq = this.FlatDistanceSquared(entry.actual, entry.placedAt);
            let stepSq = this.FlatDistanceSquared(entry.actual, entry.lastActual);
            if lagSq > 1.0 && stepSq < 0.0025 {
                if entry.stuckSince < 0.0 {
                    entry.stuckSince = now;
                }
            } else {
                entry.stuckSince = -1.0;
            }
            let settled = now - entry.foundTime > 3.0;
            if settled && entry.stuckSince >= 0.0 && now - entry.stuckSince > 2.0 {
                let behind = FloatToStringPrec(Vector4.Distance(entry.actual, entry.placedAt), 1);
                this.PlacementFailed(entry, entity, pose, female, s"the body didn't follow (\(behind) m behind)", now);
                return;
            }
        }

        let moved = Vector4.Distance(pose.position, entry.placedAt);
        let drift = Vector4.Distance(entry.actual, pose.position);
        let turned = AbsF(this.AngleDelta(entry.placedYaw, pose.yaw));
        // AI teleports go through the AI's command queue, so at most ten a second.
        let due = !this.NeedsAI(entry.method) || now - entry.lastPlaceTime > 0.1;
        if due && (entry.placements == 0 || moved > 0.01 || turned > 0.5 || drift > 0.1) {
            let coop = GameInstance.GetCoopSystem();
            if !coop.PlaceEntity(entity, pose.position, pose.yaw, entry.method) {
                this.PlacementFailed(entry, entity, pose, female, coop.GetPlacementError(), now);
                return;
            }
            entry.placedAt = pose.position;
            entry.placedYaw = pose.yaw;
            entry.lastPlaceTime = now;
            entry.placements += 1;
        }
    }

    // The current placement method doesn't work for this body: the next one, or AI walking after the last.
    private func PlacementFailed(entry: ref<CoopPuppetEntry>, entity: ref<Entity>, pose: CoopPuppetPose, female: Bool, reason: String, now: Float) -> Void {
        let coop = GameInstance.GetCoopSystem();
        let next = this.NextMethod(entry.method);
        if next != 0 {
            let from = coop.GetPlacementMethodName(entry.method);
            let to = coop.GetPlacementMethodName(next);
            entry.methodNote = s"\(from) failed: \(reason)";
            entry.method = next;
            entry.placements = 0;
            entry.stuckSince = -1.0;
            entry.aiTries = 0;
            entry.aiTryTime = 0.0;
            this.ShowStatus(s"player \(entry.peer)'s puppet: \(from) placement failed (\(reason)), trying \(to)");
            return;
        }
        if this.CanWalk(entity) {
            this.FallBackToAI(entry, entity, reason, now);
            return;
        }
        if now - entry.spawnTime > 10.0 {
            entry.lastRespawn = s"\(reason) and can't walk";
            this.Respawn(entry, pose, female);
        }
    }

    private func FirstMethod() -> Int32 {
        if this.m_placeMethod != 0 {
            return this.m_placeMethod;
        }
        return 2; // transform
    }

    // Auto: transform, then AI teleport, then the teleportation facility (which only has a chance with extra
    // components switched off, coop.ini [puppet] switchOff). A method set in coop.ini has no next one.
    private func NextMethod(method: Int32) -> Int32 {
        if this.m_placeMethod != 0 {
            return 0;
        }
        if method == 2 {
            return 3;
        }
        if method == 3 {
            return 1;
        }
        return 0;
    }

    private func NeedsAI(method: Int32) -> Bool {
        return method == 3;
    }

    private func CanWalk(entity: ref<Entity>) -> Bool {
        let puppet = entity as ScriptedPuppet;
        if !IsDefined(puppet) {
            return false;
        }
        return IsDefined(puppet.GetAIControllerComponent());
    }

    private func FlatDistanceSquared(a: Vector4, b: Vector4) -> Float {
        let dx = a.X - b.X;
        let dy = a.Y - b.Y;
        return dx * dx + dy * dy;
    }

    private func SwitchAI(entry: ref<CoopPuppetEntry>, entity: ref<Entity>, on: Bool) -> Void {
        let coop = GameInstance.GetCoopSystem();
        if !IsDefined(coop) {
            entry.aiNote = "plugin not reachable";
            return;
        }
        if coop.SetPuppetAI(entity, on) {
            entry.aiOff = !on;
            entry.aiNote = on ? "switched on" : "switched off";
            return;
        }
        // A player body has no AI controller (round J); the plugin still switched its movement component, which is
        // all placement needs.
        let puppet = entity as ScriptedPuppet;
        if IsDefined(puppet) && !IsDefined(puppet.GetAIControllerComponent()) {
            entry.aiOff = !on;
            entry.aiNote = "no AI controller (player body), movement component switched";
            return;
        }
        entry.aiNote = "the plugin couldn't switch it, see the dev panel's animation status";
    }

    private func FallBackToAI(entry: ref<CoopPuppetEntry>, entity: ref<Entity>, reason: String, now: Float) -> Void {
        entry.direct = false;
        entry.fallback = reason;
        entry.fallbacks += 1;
        entry.fallbackTime = now;
        entry.command = null;
        entry.farSince = -1.0;
        entry.stuckSince = -1.0;
        if entry.aiOff {
            this.SwitchAI(entry, entity, true);
        }
        let retry = entry.fallbacks < 2 ? " (tries direct drive again in 60 s)" : " (for the rest of the session)";
        this.ShowStatus(s"player \(entry.peer)'s puppet: back to AI walking, \(reason)\(retry)");
    }

    private func RetryDirect(entry: ref<CoopPuppetEntry>) -> Void {
        entry.direct = true;
        entry.method = this.FirstMethod();
        entry.methodNote = "";
        entry.command = null;
        entry.aiTries = 0;
        entry.aiTryTime = 0.0;
        entry.placements = 0;
        entry.stuckSince = -1.0;
    }

    // Signed shortest difference b - a in degrees.
    private func AngleDelta(a: Float, b: Float) -> Float {
        let d = b - a;
        if d > 3600.0 || d < -3600.0 {
            return 0.0; // not a real angle (infinite); never loop on it
        }
        while d > 180.0 {
            d = d - 360.0;
        }
        while d < -180.0 {
            d = d + 360.0;
        }
        return d;
    }

    // --- AI walking (drive=ai, or the fallback) --------------------------------------------------------

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
