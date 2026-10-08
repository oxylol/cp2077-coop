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

**Decided:** two-instance testing is the main loop for anything that needs two real games. The second game started from the same folder becomes dev instance 2 automatically (own player name "V 2" and id). Both games share the save folder; see [07 T0](07-testing-guide.md#t0-before-every-test-session).

## Round D (October 8)

### S2c — the plain NPC body as a target

- **Confirmed:** unlike the lookalike, the plain NPC body (`Cp2077Coop.Character.RemotePlayer`) has a visible-object component and senses. Making a gang member and it hostile to each other per agent is enough: they attack each other, no threat injection needed.
- **Confirmed, and a problem:** in a session, when V is attacked, a plain-NPC puppet joins the fight and attacks on its own (it's player-aligned and has its own combat AI). A puppet must only do what its player does. → S2d (senses off), then, if needed, its combat behaviour switched off.

### S1c — the impostor

- `gameImpostorComponent` has `isCharacterReplica`, `addHead`, `ignorePlayerHeadSlot` and `slotIDsToOmit` (a list of attachment slots). [inference] `slotIDsToOmit` lists slots the lookalike does not copy from V; if so, a lookalike could copy V's body but take weapons and clothes from the remote player's items. → round E.
- Switching the impostor off: result not reported yet.

### Delay of AI-walked puppets

The tester finds AI-walked puppets lag noticeably. Sources, largest first: the follow bots themselves trail V by 2.5–4 m and move at most 5 m/s by design (so a bot test exaggerates it); the AI walks at the animation's speed and re-targets in steps; the network buffer (~0.1 s). Tuned: 0.5 s lead, re-targeting up to 5×/s, catching up from 3 m behind. The better long-term answer is a "direct drive": position set every frame and the animation fed from the network, which is what round E starts to test (S3d).

## Round E (skipped)

Not run: instead of comparing ways to move a puppet, the direct drive was built straight away (version 0.5, [01 §4](01-architecture.md#4-remote-players-as-entities)): the game's third-person V, its AI switched off, placed every frame, animated with the remote V's captured animation inputs, falling back to AI walking by itself if placements don't move it. Its open questions (S3d "AI off + teleport", S3c "animation inputs on a player body") are now round F. The other round E probes (S2d passive puppets, S1c impostor settings) stay in the dev panel's Probes tab.

## Round F (October 8): direct drive, first try

Version 0.5.1, one game, mirror test and probes.

- **Confirmed, and a blocker:** the teleportation facility does **not** move a spawned third-person V, with its AI on *or* off. The mirror stayed where it spawned, and the probe "AI off, teleport every frame for 4 s" moved 0.00 m in four tries (121 teleports each). The same call moves cars smoothly (S1v) and V. → The bridge no longer teleports puppets; the plugin offers other placement methods (Codeware's `Entity.SetWorldTransform`, an `AITeleportCommand` through the NPC's AI), and the dev panel tests them all automatically (round G).
- **Confirmed:** the plugin's AI switch works (`puppet AI switched off` four times; RTTI call of `IComponent.Toggle`). The mirror body appears 0.1 s after it is created.
- **Confirmed (S1c):** setting the lookalike's impostor `slotIDsToOmit` to `WeaponRight, WeaponLeft` and toggling the impostor off and on stops it from copying V's weapons. So a lookalike can keep the local V's body and face while holding the remote player's own weapon (items can be put in its hands, round C).
- **Failed:** animation capture didn't start: `engine tables not found`. The handler table address from RED4ext.SDK (`CBaseFunction_Handlers`), read as "pointer to the table" as the SDK does, is null on 2.31. → 0.5.2 checks both readings (pointer to the table, or the table itself) against the native functions of `gameObject`, which must land in the game's code, and uses the one that fits; the panel shows the scores. It also captures at the animation controller itself (`entAnimationControllerComponent.ApplyFeature`, `SetInput*`, `PushEvent`, the functions every animation input from a script ends in, per the round F dump), and falls back to the replication leftovers only if those aren't there.
- From the dump (`probe-anim-components.txt`): V's entity has `entAnimatedComponent`s named `root`, `deformations`, `shadow` and `face_rig`; `entAnimatedComponent` has native `SetLocalPosition` / `SetLocalTransform`. The lookalike has `moveComponent`, `movePoliciesComponent` and `moveMotionPlannerComponent` besides its AI (round C component list): likely what keeps it in place.

## Round G (October 8): which placement moves a body

Version 0.5.2, one game.

- **Confirmed:** the placement test moved the third-person V with **transform** (Codeware `SetWorldTransform`) and with **teleport** in single 3 m steps; the **AI teleport** works too but looks choppy. Since a teleport *every frame* moved nothing in round F, a teleport probably only lands when it isn't replaced by the next one a frame later [inference]; transform sets the position at once, so it is the default (`[puppet] place=auto` tries it first).
- **Capture started:** the handler table is the table itself, not a pointer to it (`as pointer 0/31, as array 30/31 -> array`), and 6 capture points were routed on `entAnimationControllerComponent` (`ApplyFeature`, `SetInput*`, `PushEvent`). **But they got 0 calls**, from V or anyone: scripts don't call the controller's natives. The game's `AnimationControllerComponent.ApplyFeature/SetInputFloat/PushEvent` helpers are script functions that queue `AnimInputSetter*` / `AnimExternalEvent` events on the entity (`entAnimInputSetterFloat {key, value}` and so on in RED4ext.SDK), and the player's state machines use `gamestateMachineGameScriptInterface.SetAnimationParameter*` / `PushAnimationEvent` (natives, in `anim-functions.txt`). → 0.5.3 captures there: the state machine natives, and `Entity.QueueEvent` filtered to animation events on the local V.

## Round H (October 8): capture works, the mirror doesn't animate

Version 0.5.3, one game.

- **Placement, exactly** (placement test, twice): teleport and transform move the third-person V **only with its movement component (`moveComponent`) switched off** (3.00 m, stays); with nothing or only the AI off, 0.00 m. The AI teleport moves it with everything on, but choppily. The mirror with teleport + `moveComponent` off followed 100% of frames. → `coop.ini [puppet] switchOff=moveComponent` is now the default, and the plugin switches it off even for bodies without an AI.
- **Capture works:** 847 inputs in a short session. `gamestateMachineGameScriptInterface.SetAnimationParameterFeature` 667 calls from V, `…Float` 56, `…Bool` 14, `PushAnimationEvent` 80, `Entity.QueueEvent` 30 from V (7634 for others, 1313 non-animation events). The controller natives only see other characters. `anim-record.txt` has, among others: features `LocomotionStateMachine` (`AnimFeature_PlayerLocomotionStateMachine { inAirState }`), `WeaponHandlingData`, `MeleeData`, `AnimFeature_AimPlayer`, `ZoomAnimData`, `Landing { type, impactSpeed }`, `PlayerCoverActionState`, `CombatData`, `WeaponSprintBlock`; floats `crouch`, `safe`; bool `has_scope`; events `StandEnter`, `Dodge`, `Slide`, `InAir`, `Jump`, `Land`, `Shoot`, `SwitchFiremode`.
- **Nothing carries walking speed or direction:** the player's locomotion speed reaches V's graph natively (from V's own movement), not through scripts. A placed body needs it as motion inputs.
- **The mirror doesn't animate** with these inputs applied (through the controller natives in 0.5.3). Open: does its graph (the cutscene lookalike's) have these inputs at all? → 0.5.4 dumps the graphs' variables and AnimFeature slots, applies inputs the way the game's scripts do (queued AnimInputSetter events), lets the mirror be other V bodies, and has test buttons (crouch, Jump).

## Round I (October 8): the player bodies animate, but show only a neck

Version 0.5.4, one game, the mirror as each of six bodies, graph dumps (`anim-graphs-*.txt`).

- **Confirmed:** the PlayerPuppet bodies (`Character.TPP_Player`, `Character.Player_Puppet_Photomode`, `Character.Player_Replacer_Puppet_Base`) use **V's own root animation graph** (path hash `7c8ef3ac7af95509`, the same as the local V's) and **animate with the captured inputs**: the mirror moves like V.
- **The catch:** those bodies show **only a neck**. Their body and head come from somewhere they don't have when spawned this way [inference: V's look on these records is put together by the player's own systems (the player's appearance and garment setup), which a spawned copy doesn't run].
- **The cutscene lookalike** (`TPP_Player_Cutscene_…`) is the only body that looks like V (its impostor component copies the local V, round C), but it uses **another graph** (`c672de6d7a8d01f1`) and doesn't animate with V's inputs. The plain NPC body and the lookalike without an impostor don't animate either.
- **Motion input names:** the graph dumps have the float variables `speed_horizontal`, `move_direction`, `speed_vertical` and `rotation_speed_yaw`; 0.5.5 sends the motion values (worked out from the placements) into them by default.

→ 0.5.5 tries both ways of getting an animating body that looks like V, both while the body is being built (Codeware's `Entity/Initialize` callback, for entities tagged `Cp2077Coop.Puppet` or `CoopMirror`):
1. **an impostor added** to a player body that has none (set up like the lookalike's: a character replica with its own head), so a PlayerPuppet body copies the local V's look while keeping V's graph;
2. **V's graph given** to the lookalike's root animated component (off by default).

## Round J (October 8): the impostor doesn't dress a player body

Version 0.5.5, one game (read from the game folder: `red4ext/logs`, `probe-results.txt`).

- **Confirmed:** Codeware's callback works: `body setup: listening for puppet and mirror bodies being built`. The mirror spawned as `TPP_Player` (with an impostor asked for), the lookalike, the replacer, the no-impostor lookalike.
- **Still only a neck:** `TPP_Player` with "Copy my look onto it" looked the same. Whether the impostor was actually added isn't in the log (0.5.5 counted it only in the panel's status line); 0.5.6 logs one line per body. Either way, an impostor alone doesn't make a player body look like V. The option is now off by default.
- **Not tested:** the lookalike with V's graph (the graph option was off whenever the lookalike was spawned).
- The player bodies have no AI controller (`puppet AI switch: the body has no AI controller`); switching their movement component off is enough for placement.
- **Why a neck** [inference]: in round I's component dump, V's look is made of meshes that items bring: a morph-target head (`he_000_pma__basehead` and its parts), the body as a garment mesh (`t0_000_pma_base__full`), hair, beard, every piece of clothing, the arm cyberware, plus the character-customization controllers. Round C found `Items.PlayerMaTppHead` and `Items.PlayerFppHead` in V's inventory and `Items.PlayerMaTppHead` on the lookalike. A spawned `TPP_Player` gets no items, so only the neck (part of the body template) shows.

→ 0.5.6: the dev panel dresses the mirror in V's items (every item in V's attachment slots except weapons, the first-person head swapped for the third-person one), lists both bodies' slot items and components (`probe-looks.txt`), and puts single items on it for trying.

## Round K (October 8): dressed, crouching, not running

Version 0.5.6, one game (read from the game folder).

- **Confirmed:** dressing works. The mirror as `TPP_Player` got all 10 of V's slot items besides the weapon (`Chest`, `Eyes`, `Feet`, `Head`, `Legs`, `RightArm` holstered fists, `SystemReplacementCW` Sandevistan, `Torso`, `TppHead` head, `UnderwearBottom`) and looks like V: head, face, hair, body, clothes.
- **Headgear missing:** the `Head` item (a helmet) went on, but doesn't show. The body swaps its third-person head for the first-person one by itself: `Items.PlayerMaTppHead` put in `TppHead`, one second later the slot holds `Items.PlayerFppHead`. [inference] The player body treats itself as first person, which is also when the game hides V's headgear (camera clipping). Candidates: its `gameTPPRepresentationComponent`, its `gameFPPCameraComponent`.
- **Crouch animates, running doesn't.** The named float inputs (`speed_horizontal` and the others) don't move the legs. V's root graph has the feature `playerLocomotion` (`animAnimFeature_PlayerMovement`: movement and facing direction, speed, desired and stabilized speed, acceleration, strafe yaw, yaw speed, vertical speed, horizontal movement angle, in air), which the player's movement sets natively, never through scripts. → 0.5.7 builds it from the motion values (world-space directions) and sends it every frame with the other inputs; switchable in the dev panel and `coop.ini [anim] movementFeature`.
- Dressing a second time on a dressed body: 9 of 10 `AddItemToSlot` calls return false (slots taken); harmless.
- The lookalike was spawned with V's graph (`body setup: NPCPuppet (110 components): impostor off; graph swapped to V's`); no result reported yet.
- **Tester's idea:** the lookalike's look on a normal NPC body. The plain NPC's root graph (round I dump) walks through the feature `locomotion` (`animAnimFeature_Locomotion`: action, style, path curvature, …), which its AI movement drives; a placed NPC would need that fed the same way, and its graph doesn't take V's other inputs (crouch, weapon, jump). Kept as the fallback if the player body can't be made to run.

## Round L (October 8): the legs run; the body is a first-person woman

Version 0.5.7, one game (read from the game folder).

- **Confirmed:** with the built `playerLocomotion` feature the dressed `TPP_Player` runs with correct legs, and the arms swing ("jog"). Without it (panel checkbox off) the legs stay still. So V's graph takes its walking from that feature, and world-space directions work.
- **Still wrong on the player body:** the torso is "very buggy"; no head; it's a **woman's** body (`probe-looks.txt`: `t0_000_pwa_base__full_shadow`, `n0_000_pwa_fpp__neck`, `pwa` clothes, while V is `pma`), and none of V's face, hair or body features. `TppHead` again ends up holding `Items.PlayerFppHead`.
- **The cutscene lookalike** has all of V's looks (the impostor copies the local V) but doesn't animate.
- **Crash:** giving an NPC body (the lookalike, the no-impostor lookalike) V's root animation graph crashes the game right after the swap (twice: the log ends at `graph swapped to V's`). Player bodies already have V's graph, so the option never changed them. → removed in 0.5.8.
- [inference] Everything wrong with the player body fits one cause: it behaves as a **first-person** body. On the real V the game switches between first- and third-person representation with the `gameTPPRepresentationComponent` (events `gamePrepareTPPRepresentationEvent` → `gameAppearancesReadyTPPRepresentationEvent` → `gameFinalizeActivationTPPRepresentationEvent`, `gameDeactivateTPPRepresentationEvent`; quest node `EntityManagerEnablePlayerTPPRepresentation`); it has a slot listener (which would explain the head swap) and a character-customization state updater (face, hair, body). V's root graph has the feature `TPPRepresentation` (`gameAnimFeature_TPPRepresentation { IsActive }`).

→ 0.5.8: the dev panel sends the mirror the prepare and finalize events after it appears, the plugin sends `TPPRepresentation { IsActive = true }` with the motion inputs (`coop.ini [anim] tppFeature`), and the mirror can be spawned with V's appearance name (Codeware `DynamicEntitySpec.appearanceName`); `List looks` writes both bodies' appearance and template.

## Round M (October 8): the player body stays a first-person woman

Version 0.5.8, one game (read from the game folder).

- **No effect:** the TPP representation events (`gamePrepareTPPRepresentationEvent`, then `gameFinalizeActivationTPPRepresentationEvent`, both queued without error): `TppHead` still ends up holding `Items.PlayerFppHead`, still female, no head, none of V's face or hair (the tester sees the default female V hair). V's appearance name is `None` (player bodies don't use one), so spawning with it changed nothing.
- **T-pose:** sending `TPPRepresentation { IsActive = true }` puts the player body in a T-pose with no animation. → off by default (`[anim] tppFeature=false`).
- From Codeware's list of known resource paths: the player's third-person templates are `base\characters\entities\player\player_ma_tpp.ent` / `player_wa_tpp.ent`, the lookalikes `player_ma_tpp_cutscene.ent` / `…_cutscene_no_impostor.ent`, and there is a mirror-reflection body `player_ma_tpp_reflexion.ent` (plus `ep1\characters\entities\player\player_ma_tpp_ep1.ent`). `Character.TPP_Player` spawns a female body, so it most likely points at `player_wa_tpp.ent`.

**Decision (tester):** work on the cutscene lookalike, which has V's whole look (gender, face, hair, clothes) through its impostor; it is an `NPCPuppet` whose root graph (`c672de6d7a8d01f1`) has the same kind of variables as V's (`speed_horizontal`, `desired_speed_horizontal`, `move_direction`, `crouch`, `sprint`, `jump`, …) but reacted to none of them in any round, and slid under AI walking in round C. [inference] It has no walk animations to play: cutscene bodies get their animations from scenes.

→ 0.5.9: the graph dump also lists each animated component's rig and animation sets (gameplay and cinematic, by path hash) and the animation setup extensions and graph tables, so the lookalike's sets can be compared with the player body's; an option lends an NPC body the local V's gameplay animation sets while it is built (`[puppet] borrowAnimsets`, panel checkbox; may crash); the speed also goes to `desired_speed_horizontal` (motion names can be joined with `+`).

## Round N (October 8): the lookalike's graph doesn't walk

Version 0.5.9, one game; dumps read from the game folder.

- **The lookalike** (`TPP_Player_Cutscene_Male`): root rig `c5f417c6257b385d`, graph `c672de6d7a8d01f1` with 4 gameplay animation sets, plus an animation setup extension named **"Player TPP Animation Setup"** (3 sets, one with 65 variable names), `man_face_base_animations` (57 face sets) and `ui_animations`. So it does carry third-person V animation sets.
- **The player body** (`TPP_Player`): root rig `ba775c3ad74a1dbe` (the female third-person rig), V's graph `7c8ef3ac7af95509`, 101 gameplay sets, `CarAnimsets` (28).
- **V** (first person): root rig `655fc64ff60fef6c`, the same graph, 102 different gameplay sets (first person), `EP1 animsets` (20).
- **Lending** V's 201 gameplay sets to the lookalike: logged (`animation sets borrowed: 201`), no crash, **no change**: still frozen. They are first-person sets for another rig.
- The lookalike keeps `Items.PlayerMaTppHead` in `TppHead` (an NPC body doesn't swap heads); the player body swaps it.
- [inference] The lookalike's graph is made for scenes: it has the animations but no walking logic that reads `speed_horizontal` and the like (or reads them only inside scene-driven states). Making it walk would mean changing its graph, which crashed in round L.

**Conclusion:** the lookalike's *look* comes from its impostor, the player body's *walking* from V's graph and the `playerLocomotion` feature. Codeware's known resource paths list the male third-person player body as its own template, `base\characters\entities\player\player_ma_tpp.ent` (`Character.TPP_Player` gives the female one), and a mirror-reflection body `player_ma_tpp_reflexion.ent`. → 0.5.10 can spawn a body from a template path (`CoopSystem.SetSpawnTemplate` writes the path hash into Codeware's `DynamicEntitySpec.templatePath`); round O tries the male player body with the lookalike's impostor.

## Round O (October 8): template spawns crash; the project builds on CyberpunkMP

Version 0.5.10, one game.

- **Crash:** the mirror spawned as `template:…\player_ma_tpp.ent` crashed the game, and so did `…\player_ma_tpp_reflexion.ent`. Both were spawned by path (`CoopSystem.SetSpawnTemplate` → Codeware `DynamicEntitySpec.templatePath`), the only bodies of all rounds spawned without a TweakDB record.
- **Cause** [inference, strongly supported]: Codeware turns a template spawn into a bare `SpawnableObject` record holding only the template path (`ConvertTemplateToRecord` in its `DynamicEntitySystem.cpp`), and its documentation warns "NPCs and vehicles may not function properly if spawned using template". A `PlayerPuppet` built without a Character record crashes. Every body that spawned fine (rounds A–N) came from a Character record.
- **Decision (project owner):** the project becomes an enhancement of CyberpunkMP ([01 §1](01-architecture.md#1-foundation-cyberpunkmp), [LICENSE.md](../LICENSE.md)); its code was read for the first time. What it does for remote players:
  - **Body:** always a Character record. Its `CyberpunkMP.tweak` has `Character.Muppet : Character.TPP_Player` with `genders` pointing at `player_wa_tpp.ent` / `player_ma_tpp.ent`, and the ones it uses, `Character.MaMuppet` / `WaMuppet`, are based on `Character.Panam` (an NPC) with its own edited copy of the cutscene lookalike template (`mods\cyberpunkmp\player_ma_tpp_cutscene.ent`, shipped in its archive).
  - **Look:** each client serializes its character customization state (the game's own stream format, ~13 KB) and sends it with the visual items of its paper-doll slots. The receiver gives the body the items (with garment appearance names that include the `&TPP` suffix), sets a byte in the body's `gamePuppetPS` that the game checks when it resolves `&TPP` appearances, and applies the state's third-person head, face, hair, beard, body and arms parts through the world's entity appearance changer (`ScheduleSynchronizedAppearanceChanges`).
  - **Movement:** its own movement controller on the NPC's move component, feeding the NPC locomotion feature only idle, walk and sprint (with the animation time); no crouch, jump or weapons.
- **What this project takes** (0.6): the record-based body (male and female player templates through `genders`, `tweaks/Cp2077Coop/bodies.tweak`) and the whole look pipeline (`src/plugin/Looks.cpp`). It keeps its own animation: the player body has V's graph, which round L showed runs with the `playerLocomotion` feature, and the captured inputs add crouch, jump and weapons, which CyberpunkMP's controller doesn't have. The third-person byte is the best candidate yet for rounds K–M's first-person head swap.
- The reflection body (`player_ma_tpp_reflexion.ent`) is dropped: it is the game's mirror-reflection double of the local V, of no use for a remote player.

## Round P (pending): the player body with your look

Steps in [07 T4m](07-testing-guide.md#t4m-the-player-body-with-your-look-round-p). Questions:

1. Does `Cp2077Coop.Character.PlayerBody_Male/_Female` spawn without crashing, as a body of your V's gender?
2. With your look applied (third person, items, customization): your V's face, hair, head, body and clothes? Headgear? Does the head stay third person (`TppHead`)?
3. Does it still walk, run, crouch and jump with you (V's graph, `playerLocomotion`)?
4. Which of the plugin's game functions are found on 2.31 (status line `looks (CyberpunkMP): … game functions: …`), and what does the `look:` line in `red4ext/logs` report?
5. With fake players: are their puppets player bodies with your look?
