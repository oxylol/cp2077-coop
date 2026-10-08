# 08 — Spike results

What the in-game experiments ([04 §3](04-feasibility-and-risks.md#3-reverse-engineering-spikes-do-these-first)) have shown so far, and what each result decided. Game version 2.31, one PC, probes from the CET dev panel (`cet/coop-dev`). Raw outputs (`probe-*.txt`) stay with the tester; they are RTTI dumps of the game and aren't committed.

Marks: **confirmed** = seen in game; [inference] = our reading of a result, not yet checked; [VERIFY] = open.

## Rounds A and B (October 2026)

### S1 — what can look like V

- 11 candidate records found (`Character.TPP_Player`, `TPP_Player_Cutscene_Male/Female`, `…_No_Impostor_…`, `Player_Puppet_*`, `Silverhand`, …).
- **Confirmed:** `Character.TPP_Player_Cutscene_Male` spawned through `DynamicEntitySystem` looks exactly like the local V, and holds whatever weapon the local V holds: it switches instantly when V switches, without a draw animation.
- The character customization system has one state (`gameuiCharacterCustomizationSystem.GetState()`), which would explain why every V-lookalike copies the local V [inference].

**Decided:** puppets use `TPP_Player_Cutscene_Male/Female` (in `CoopBridge`; round C changed the pick to the local V's body, see below). For now a puppet shows the *local* V's face, body, clothes and weapon: a known compromise until S1b.

**Open:** does this record walk with AI commands? (S3 was run on the placeholder NPC.) → S3b. How does the copying work, and can a puppet show the remote player's look and items instead? → S1b.

### S1 vehicles

- **Confirmed:** a vehicle spawns through `DynamicEntitySystem`; `TeleportationFacility.Teleport` moves it exactly (5.00 m asked, 5.00 m moved).
- **Confirmed:** `AIMountCommand` (`gameMountEventData` with `slotName = seat_front_right`, `isInstant`, `ignoreHLS`) seats an NPC as a passenger.
- **Confirmed:** with an NPC passenger in the attitude group `player`, V gets into the driver's seat normally. Otherwise V has to take the car by force and the NPC is thrown out.
- The dumps show what vehicle capture needs: `GetRecordID`, `GetCurrentAppearanceName`, `IsPlayerDriver`, `GetSlotIdForMountedObject`, `GetLinearVelocity`, `GetCurrentSpeed`, and `SetVehicleRemoteControlled` [VERIFY what it does to a proxy car].

**Decided:** proxy cars are moved by teleport; puppets are player-aligned so the local V can share a car with them.

**Open:** is a teleport every frame smooth enough for a car someone else drives? Does a seated NPC move with a teleported car? Can the local V be seated as a passenger by script? → S1v-b.

### S2 — puppets as AI targets

- **Confirmed:** an NPC in the attitude group `player` standing next to a gang during a fight is ignored completely. It also doesn't react itself.
- [inference] Hostility in a fight is per agent (the gang turns hostile towards V's agent), not per group, and/or the NPC isn't registered as a possible target.

**Open:** what does the gang's attitude towards the puppet look like, can it be made hostile per agent, and can the puppet be injected as a threat? → S2b. Fallback if none works ([04 §5](04-feasibility-and-risks.md)): owner-side forced targets.

### S3 — moving an NPC

- **Confirmed:** `AIMoveToCommand` (target `WorldPosition` → `AIPositionSpec.SetWorldPosition`, `movementType = Walk`, `ignoreNavigation`) makes a spawned NPC walk to V with its normal animation.
- **Confirmed (round A):** `TeleportationFacility.Teleport` doesn't move spawned NPCs (147 teleports, no movement). [VERIFY] why; the round C dump looks for an AI-side teleport command.

**Decided:** player puppets walk by AI move commands instead of running with AI off ([01 §4](01-architecture.md#4-remote-players-as-entities)). A puppet that falls far behind is respawned at the right place.

### S8 — time dilation

- **Confirmed:** `TimeSystem.SetTimeDilation(reason, 0.25, duration)` slows everything, V included, and `UnsetTimeDilation(reason)` restores it. `SetIgnoreTimeDilationOnLocalPlayerZero(true)` keeps V at full speed while the world is slow.
- **Confirmed:** the panel's "Apply to the game" switch slows the world during a bot's Sandevistan, exempts V during V's own, and returns to normal, without crashing.
- **Crash found and fixed:** in CET, `CName.new("")` is a real name (a hash of the empty string), not the engine's empty name; only `"None"` is. Passed as an ease curve it crashed the game. Curves are now left out, or passed as `CName.new("None")`.
- The dumps show the per-entity API the design needs: `SetIndividualTimeDilation(reason, dilation, duration, easeIn, easeOut, ignoreGlobalDilation, useRealTime)`, `UnsetIndividualTimeDilation`, `IsIgnoringGlobalTimeDilation`, plus `GetActiveTimeDilation` and `SetTimeDilationOnLocalPlayerZero`.
- Not run yet: individual rate on an NPC.

**Open:** does `ignoreGlobalDilation` keep a puppet at full speed in a slowed world (how a remote Sandevistan user must look)? → S8b.

### S13 — two copies on one PC

Not run in this round; see round C.

## Round C (October 8)

### S3b / S3c — which body animates

- **Confirmed:** a female lookalike (`TPP_Player_Cutscene_Female`) doesn't spawn while the local V has a male body; two male lookalikes can exist at once.
- **Confirmed (lineup, same AI move command for all):** `TPP_Player_Cutscene_Male` (looks like V) and `TPP_Player_Cutscene_No_Impostor_Male` (a naked body without a head) move but slide without animating. `TPP_Player`, `Player_Puppet_Photomode` and `Player_Replacer_Puppet_Base` don't move at all (they have no AI component; their components are the player-puppet kind, with quick slots). The plain NPC walks, runs and sprints with full animation.
- The component dump shows why: the plain NPC carries `Gameplay Animation Setup`, `Special Locomotion Setup` and body-type animation sets, which the lookalikes lack; they carry `Player TPP Animation Setup` instead. Both have an `entAnimationControllerComponent` with `SetInputFloat/Int/Bool/Vector`, `PushEvent` and `ApplyFeature`, so a player body's animation could in principle be driven from the network, once the inputs its graph reads are known.
- Bridge bugs found on the way, fixed: the spawn timeout was too short, and a "far behind" respawn fired the moment a body appeared (a fresh body reports a wrong position at first [inference]).

**Decided:** session puppets use the plain NPC body (animated, doesn't look like V); the lookalike is opt-in in `coop.ini`.

### S1b — how the lookalike gets V's looks

- **Confirmed:** the lookalike has a `gameImpostorComponent`: that's the part that copies the local V (the `No_Impostor` record has none and is bare). It copies live: when V changes weapons or clothes in the inventory, the lookalike follows.
- **Confirmed:** items can be put on it: `TransactionSystem.GiveItem` + `AddItemToSlot` made it hold a Lexington that V didn't hold, until V switched weapons and the copy took over again.
- Its own inventory holds `Items.PlayerMaTppHead`: the head is an item, which is why the bare body has none.
- The customization state interface (`gameuiICharacterCustomizationState`) exposes body and brain gender, life path and attributes, not the face options; those come from the system's option lists (`GetHeadOptions` and friends).

**What this means for co-op:** on your screen, a lookalike would show *your* V's face, clothes and weapon on every other player. Each player's looks must come from that player's own game over the network, so the copying is in the way. The promising route is the bare body plus that player's items (head, hair, clothes, weapon); the face customization is the hard part. Both depend on a player body that animates (above).

### S2b — puppets as AI targets

- **Confirmed:** the lookalike has no visible-object component and no senses (so NPCs can't perceive it on their own), but it has a target tracker.
- **Confirmed:** making a gang member and the lookalike hostile to each other (`AttitudeAgent.SetAttitudeTowards`) is not enough. Giving the gang member the lookalike as a threat (`TargetTrackingExtension.InjectThreat` together with the target tracker's `AddThreat`) makes it attack.

**Decided:** for combat (M1), the machine that simulates an NPC injects remote players' puppets as threats, on top of per-agent hostility. Still to check: which of the two threat calls is needed, and whether the plain NPC body (which has senses) is perceived without help.

### S8b — a puppet at its own speed

- **Confirmed:** `SetIndividualTimeDilation(reason, 1.0, duration, None, None, ignoreGlobalDilation = true)` keeps an NPC walking at full speed while the world and V are slowed to x0.25; everything returns to normal afterwards. No crash with `CName.new("None")` as the curves.
- Earlier the same day: `SetIndividualTimeDilation(reason, 2.0, 3 s)` on a probe ran without problems.

**Decided:** time fields are now applied by the mod itself (`CoopBridge`), no longer by the dev panel: world rate, V exempt while activating, and puppets at their players' own rates. `coop.ini` `[time] applyToGame=false` turns it off.

### S1v-b — vehicles

- **Confirmed:** an NPC seated as driver (`seat_front_left`) sits in the car; a teleport every frame drives a car 15 m in 91 steps smoothly; the tester reports everything working as intended.

**Decided:** proxy cars are driven by a teleport per frame, with puppets seated by `AIMountCommand`. The game side of vehicles is next.

### S13 — two copies on one PC

- **Confirmed:** starting the game a second time just works (Steam, patch 2.31).

**Decided:** two-instance testing is the main loop for anything that needs two real games. The second game started from the same folder becomes dev instance 2 automatically (own player name "V 2" and id). Both games share the save folder; see [07 B0](07-testing-guide.md#b0-before-every-test-session).

## Round D (October 8)

### S2c — the plain NPC body as a target

- **Confirmed:** unlike the lookalike, the plain NPC body (`Cp2077Coop.Character.RemotePlayer`) has a visible-object component and senses. Making a gang member and it hostile to each other per agent is enough: they attack each other, no threat injection needed.
- **Confirmed, and a problem:** in a session, when V is attacked, a plain-NPC puppet joins the fight and attacks on its own (it's player-aligned and has its own combat AI). A puppet must only do what its player does. → S2d (senses off), then, if needed, its combat behaviour switched off.

### S1c — the impostor

- `gameImpostorComponent` has `isCharacterReplica`, `addHead`, `ignorePlayerHeadSlot` and `slotIDsToOmit` (a list of attachment slots). [inference] `slotIDsToOmit` lists slots the lookalike does not copy from V; if so, a lookalike could copy V's body but take weapons and clothes from the remote player's items. → round E.
- Switching the impostor off: result not reported yet.

### Delay of AI-walked puppets

The tester finds AI-walked puppets lag noticeably. Sources, largest first: the follow bots themselves trail V by 2.5–4 m and move at most 5 m/s by design (so a bot test exaggerates it); the AI walks at the animation's speed and re-targets in steps; the network buffer (~0.1 s). Tuned: 0.5 s lead, re-targeting up to 5×/s, catching up from 3 m behind. The better long-term answer is a "direct drive": position set every frame and the animation fed from the network, which is what round E starts to test (S3d).
