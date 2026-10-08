# 02 — System-by-system design

Each system lists: the **logical model** ("what would actually happen if a second person were standing here?"), the **plan**, the **hooks** involved, a **fallback**, and its risk rating from [04-feasibility-and-risks.md](04-feasibility-and-risks.md). Engine names are best current knowledge and are marked **[VERIFY]** with how to check. Message names refer to [03-network-protocol.md](03-network-protocol.md).

How to verify hook names, in general: look the class up in NativeDB; grep the decompiled game scripts (redscript `--dump`/WolvenKit script export of `r6/cache/final.redscripts`); probe live with CET (`Game.GetXxxSystem()`, `Observe`/`Override` on the method and log calls). Natives not exposed to script: RTTI dump plus Ghidra on the 2.31 exe, located through RED4ext's address library.

---

## 1. Quest progression

### Logical model
The story belongs to the host's V. The other players are mercs in V's crew: they can be present for anything, help with anything, and take jobs on their own, but the main story happens to the host's V.

### Plan

**1a. Host executes quests; clients are followers.**
- The host's quest system runs normally.
- Every fact write on the host is captured by a native hook on the quest system's fact setter and batched into `FactDelta` (sequence-numbered, name hashes + string dictionary).
- On clients, the local quest system's **graph execution is frozen** (phases don't tick, nodes don't fire), but the fact database and journal stay live and are written only by replication. Anything in the client's world that is *fact-driven* (fact-conditioned communities, map pins, journal state) therefore follows the host automatically.
- Quest *world effects* (spawned NPCs, scene actors, door states) happen on the host's machine. Clients near the host see them as proxies because the host owns shared cells by default.

**1b. Remote triggers.** Quest conditions of the form "player enters area / interacts with X / kills Y / picks up item Z" are evaluated for **any** player. When a client satisfies one on its own machine, it sends `QuestTriggerReport`; the host injects it into its quest system as if its V had done it. Only for triggers whose content exists on the host's machine (the host is nearby) or that are pure fact checks.

**1c. Quest leasing for self-contained content.** Gigs, NCPD hustles and other minor activities, cyberpsycho sightings and side jobs that don't feed other quests can be **leased** to a client:
1. Client approaches the activity while the host isn't running it. Client sends `QuestLeaseRequest(questId)`.
2. Host checks the quest is leaseable (offline table, below), not active near the host, and not leased. Host snapshots that quest's facts and replies `QuestLeaseGrant(questId, factSnapshot)`, and keeps that quest dormant on its own machine.
3. The client unfreezes **only that quest** in its local quest system and plays it. The client's V is the protagonist; the client makes the dialogue choices.
4. Fact writes inside the lease's fact set go client → host (`FactDelta` with lease id); writes outside the set are rejected and logged.
5. On completion (`QuestLeaseComplete`), the host applies final facts and journal state; the activity is done in the host's world, and the lessee gets the reward.
6. If the lessee disconnects or abandons, the host restores the snapshot (`QuestLeaseRevoke`).

Leaseability is computed offline by `tools/coop/quest-deps`: WolvenKit CLI exports every `.quest`/`.questphase`; the tool extracts facts read and written per quest, builds a dependency graph, and marks a quest leaseable when its written facts (other than its own completion facts) aren't read by any other quest and it contains no main-story scenes. The output is a generated table shipped with the mod and regenerated per game patch.

**Client progress rule (what clients keep afterwards).**
- **Always kept:** everything that is the character: level, XP, attributes, perks, skill progression, cyberware and capacity, inventory (minus quest-tagged items), money, street cred, crafting specs, owned vehicles bought during the session, wardrobe.
- **Never kept:** main-story and world state (facts, journal, romance and relationship state, world layers).
- **Kept when matched (optional, host setting, default on):** completion of *leaseable* activities the client completed as lessee, **if** that activity was in the identical starting state in the client's own save. At join, the client records the fact signature of every leaseable quest from its own save; on return, completions are transplanted only for quests whose signature matched. This is the Seamless Co-op "same point" rule, restricted to self-contained content where transplanting facts can't break a dependent quest.

### Hooks [VERIFY]
- Fact setter: native `questQuestsSystem` `SetFact`/`SetFactStr` (script `QuestsSystem`). Check in NativeDB; hook natively (RED4ext) rather than in script so quest-graph writes are caught.
- Graph tick / phase executor: native `questQuestsSystem` update. Not script-visible. Find via RTTI vtable of `questQuestsSystem` and Ghidra; spike S5 must prove that freezing it is stable and that per-quest unfreezing (for leases) is possible.
- Journal: `gameJournalManager` (`JournalManager` in script): entry state changes and tracking. Hook state changes on host; apply on clients.
- Trigger injection: identify the node types for area triggers and interactions in questphase files (WolvenKit), then find the native condition evaluators.

### Fallback
If per-quest unfreezing is impossible, leasing degrades to "the host's machine runs every quest", and open-world activities progress only near the host. [COMPROMISE] flagged as the single largest gap versus the spec.

**Rating:** facts Easy; follower mode Research; remote triggers Hard; leasing Research.

---

## 2. Scenes, dialogue, cutscenes

### Logical model
A conversation happens in a place. People nearby can watch and hear it; people elsewhere are unaffected. Nobody is pulled in, frozen or hidden.

### Plan

**2a. Observer replay (primary).** When a scene starts on the machine running the quest (host, or lessee), that machine broadcasts `SceneStart(sceneResourceHash, instanceId, actorBindings, startTime)` to machines whose interest covers the scene's location. Each observer plays **the same scene resource locally in observer mode**:
- Actor bindings map scene performers to the observer's local instances of the same NetIds (proxies). The `player` role is bound to the **host's puppet**, never to the observer's own V.
- The observer's own V is excluded: no input lock, no camera, no workspot, no forced holster.
- Scene time is driven by `SceneSync` (section id + time) at 2 Hz; the observer slews to it.
- V's voice lines use the host V's voice variant (gender), passed in `SceneStart`.
- All audio is positional at the performers, so walking away makes it quieter.

**2b. Fallback: animation hints.** If observer replay can't be made to work, replicate per-performer animation state at 20 Hz (workspot id, animation id + time, look-at target), play VO events positionally (`VOEvent(lineHash, speakerNetId)`), and let the engine's lip-sync run from the VO. Lower fidelity (no camera-tuned facial performance), no scene-specific props unless replicated.

**Dialogue choices.** The machine running the scene hooks the dialogue choice hub data and sends `ChoiceHubShow(options[])` with localization keys plus flags (lifepath, attribute check, colored option, timed). Nearby players see a read-only panel anchored above the deciding player: "V is deciding" with the same options; the picked option highlights on `ChoiceSelected`. Only the protagonist (host for main quests, lessee for leased ones) can choose.

**Locked-movement scenes** (V seated, on a ripperdoc chair): only the protagonist is locked. Observers walk freely.

**Holocalls:** the call is on the protagonist's optical implant, so in-world only V's spoken half exists. Observers hear the protagonist's lines positionally from the protagonist's puppet; the caller is inaudible. Host setting **"Patch calls to crew"** (default off): when on, observers also get the caller's audio and the call's HUD frame, as a diegetic party line.

**Car-ride scenes:** the scene vehicle is owned by the machine running the scene. Free seats are real seats: clients can board through vehicle seat sync (§11). Seats held by scene performers are unavailable. Otherwise clients follow in their own vehicles.

**Intimate and romance scenes:** a **privacy volume** (the room or interior where the scene happens) is defined per scene in a data table (`code/assets/Tweaks/Coop/privacy_scenes.yaml`, generated from a list of romance scene resources):
- The scene doesn't start while another player is inside the volume. The protagonist sees "Waiting for privacy"; others inside see a gentle prompt "Give them some privacy." Nobody is moved.
- Once it starts, the volume's doors lock on every other machine and the volume is closed to other players' entry, until the scene ends.

**Scene blocking.** A puppet standing in a performer's path could stall the scene on the protagonist's machine. [COMPROMISE] During scenes, performers ignore collision with other players' puppets (they pass through). Pushing a player out of the way would violate "nobody is moved."

### Hooks [VERIFY]
- Scene start/stop: native scene system (`scnSceneSystem` / `gameSceneSystem`; script `GameInstance.GetSceneSystem()`). Spike S6: start a scene resource with an arbitrary entity bound to the player performer slot, without the local V being affected.
- Choice hubs: UI interactions blackboard (`DialogChoiceHubs`) and the dialog widget controller. Check blackboard defs in the script dump.
- V voice variant: how scenes pick V's gendered VO. Check `.scene` files in WolvenKit for voice-variant tagging.
- Privacy: door lock state through device persistent state (`DoorControllerPS`).

**Rating:** observer replay Research; animation-hint fallback Hard; choice mirror Hard; privacy volumes Hard.

---

## 3. Johnny Silverhand and Relic events

### Logical model
Every V in the session carries their own engram. **Each player sees and hears their own Johnny, and only their own.** Johnny is in a V's head, so another player's Johnny is never visible or audible to you; you see that player talking to empty air.

### Rules
1. **One Johnny per machine.** Each machine renders exactly one Johnny entity, and it belongs to that machine's own V. Johnny is never replicated, never a network entity, and has no puppet on other machines.
2. **Ambient Johnny** (sitting in your passenger seat, leaning on a wall, commenting on what you're doing) is driven locally for your own V. Systems that spawn him for the local player keep working on every machine, clients included.
3. **Story scenes with Johnny** (host's quest, replayed on nearby observers per §2): on the protagonist's machine he's the protagonist's Johnny as in vanilla. On each observer, the scene's Johnny performer is shown as **the observer's own Johnny**, playing the same performance. Everyone in the conversation sees "their" Johnny in the same spot saying the same thing, so the scene reads the same for all, and nobody sees two Johnnys.
4. **No duplicates.** When a replayed scene with Johnny starts within view, the observer's ambient Johnny glitches out and the scene's Johnny takes his place; he comes back as ambient when the scene ends.
5. **Leased content** (gigs, side jobs a client runs, §1c): the lessee's machine runs the quest, so Johnny's commentary in it plays for the lessee's own Johnny, exactly as in single-player.
6. **Johnny's lines are private.** Johnny's VO is heard only by the player he belongs to (head-space audio as in vanilla). The protagonist V's own spoken replies are audible positionally to people nearby.
7. **Johnny takeovers** (Johnny controlling the host's body in story missions): other players see the **host's V body**, since physically it's still V. Their own Johnny is unaffected.
8. **Relic malfunctions and collapses are story events** of the protagonist: others see that V collapse (scene replay or animation hints) and can stand by. Other players' V don't collapse from the host's story beats.
9. **Johnny's memories / flashbacks:** the protagonist's body stays in the world where the memory started; others see it there. Watching the memory itself: §16.

### Hooks [VERIFY]
- Johnny's record id(s), appearance names and the ambient-appearance system (TweakDB dump; script dump search for the ambient Johnny spawner and the passenger-seat feature): must run for the local player on every machine and must never be suppressed on clients.
- Scene performer binding for Johnny on observers: bind the Johnny role to the observer's own Johnny entity (spike S6), or spawn one if none exists.
- VO routing: Johnny lines as head-space audio for the local player only (scene VO events, speaker filter).
- Takeover appearance swap: find the system that swaps the player's appearance to Johnny and make sure the swap is never replicated to the puppet.
- Whether a client's own Johnny exists at all depends on the client's character state: [VERIFY] which facts gate Johnny's presence for a V. In the host's world those facts are the host's; decide per-player gating in spike S5 (default: Johnny appears for every player once the host's story has the Relic active).

**Rating:** Hard (depends on §2 and S6).

---

## 4. Combat

### Logical model
Every bullet, blade and fist is a real thing in one shared space. NPCs fight whoever is threatening them.

### Plan

**Hit registration.** The shooter detects hits locally against the proxies on its own screen (immediate feedback), computes **offense** (base damage, crits, weapon and perk modifiers, attack type, status-effect payloads) with its own stats, and sends `HitRequest(target, epoch, seenAtW, offense, hitZone, attackId)`. The **target's owner** rewinds to `seenAtW` (lag compensation, ≤ 400 ms), validates range/LOS/fire rate, and runs the hit through its own damage pipeline as **defense** (armor, resistances, zone multipliers, immunities). `HitResult` goes to everyone. The spec says "host-validated"; with distributed authority this is **owner-validated**, and the owner is the host in shared cells by default.

**Damage to players.** An NPC owner whose NPC hits a puppet sends `PlayerHitRequest(victim, offense)` to the victim's machine, which applies it to its real V through the vanilla pipeline (armor, perks, cyberware like Second Heart, all handled locally).

**Status effects.** Applied by the owner of the affected entity; `StatusEffectApply/Remove(netId, recordId, remaining, instigator)` replicate visuals (burning, shocked, poisoned, EMP) to everyone.

**Aggro split.** Each NPC's threat list on its owner's machine includes the local V and all puppets. NPCs pick targets by vanilla threat logic. Depends on puppets being valid targets ([01 §4](01-architecture.md#4-remote-players-as-entities)).

**Stealth and detection.** NPC senses run on the owner. Detection progress for each (NPC, player) pair above zero is replicated to that player (`Detection(npc, level, state)` at 10 Hz) so their own detection indicator works. Combat alerts propagate through the NPC's squad on the owner's machine; squads spanning owners are kept on one owner by pinning a squad to the owner of its leader.

**Takedowns and grapples.** Grabbing an NPC requests immediate ownership (`TakedownRequest`); the NPC owner grants by transferring authority to the grabber (fast path, 1 RTT), and the paired animation runs on the grabber's machine. Other machines see the grabber puppet and the NPC proxy play the paired animation (`PairedAnim(attacker, victim, animId, startTime)`). Bodies being carried are pinned to the carrier.

**Visual fire.** `WeaponFire(shooter, weaponId, muzzle, dir, seed)` lets others render muzzle flash, tracers, impacts and audio. Unreliable, with the last 3 shots repeated in each packet so drops don't lose shots.

**Explosions and area effects.** A single `AreaEffect(center, radius, effectRecord, instigator, offense)` message handles grenades, explosive barrels, Gorilla Arms slam, Berserk landing, EMP, Short Circuit chains. The originating machine sends it; **each machine applies it to the entities it owns** and to its own V.

### Hooks [VERIFY]
- Hit pipeline: `gameHitEvent` and the scripted damage pipeline (`DamageSystem` in the script dump; locate the stage where final damage is applied and where hit flags are computed).
- NPC targeting: `TargetTrackingExtension`, attitude system (`AttitudeAgent`), senses (`SenseComponent`). Spike S2.
- Status effects: `StatusEffectHelper.ApplyStatusEffect` / `StatusEffectSystem`.
- Takedown/grapple: player state-machine takedown and grapple states (PSM scripts), `TakedownUtils`.

### Fallback
If puppets can't be made full AI targets, NPCs target only the local V of their owner's machine and puppet damage is handled by owner-side "virtual aggro" (NPCs on the owner shoot at puppet positions by forcing a target override). Less natural, still consistent.

**Rating:** hits/damage Hard; aggro split Research; stealth Research; takedowns/grapples Research.

---

## 5. Quickhacks and netrunning

### Logical model
A quickhack is code uploaded from one netrunner to one target. Its effect happens to the target, wherever it is, for everyone.

### Plan
- **Targeting and upload:** the hacker's machine scans and targets the proxy, spends RAM locally, and runs the upload timer locally. `QuickhackUpload(target, hackRecord, uploadTime)` lets the target's owner show the NPC's "being hacked" reaction (and lets NPC netrunners trace back, which works because trace runs on the owner).
- **Effect:** on completion, `QuickhackApply(target, hackRecord, offense)`. The owner applies the effect with the hacker's precomputed strength. Damage-over-time, spreads (Contagion), chains (Short Circuit) and AI-altering hacks (Cyberpsychosis, Suicide, System Collapse, Memory Wipe) execute entirely on the owner, so their consequences are consistent for all.
- **Cancel / interrupt:** `QuickhackCancel` when the hacker is interrupted or loses line of sight.
- **NPC netrunners hacking a player:** owner sends `QuickhackApply` to the victim's machine; the victim's machine runs the player-side effect and counter-measures.
- **Breach protocol:** the minigame is local to the hacker. The result (`BreachResult(accessPoint/target, daemons[])`) is applied by the owner of the network devices or NPCs (daemons like camera shutdown, weakened armor, RAM cost reduction on that subnet). Per-player bonuses (RAM recovery) apply locally. Datamine rewards are instanced per player.
- **Shared or per-player:** world effects (camera disabled, turret friendly, NPC on fire) are shared, because they happen to the world. Hacker-side effects (RAM, cooldowns, upload queue) are personal.

### Hooks [VERIFY]
Quickhack upload and completion events on `ScriptedPuppet`/device PS classes; the quickhack system (search the script dump for `QuickHack` and `UploadProgram`); breach minigame completion callback (`HackingMinigame`, access point controller PS).

**Rating:** basic hacks Hard; AI-altering and spreading hacks Research; breach Hard.

---

## 6. Cyberware abilities

### 6.1 Sandevistan and Kerenzikov

The mechanism is a **time field**, specified in [01 §8](01-architecture.md#8-time-fields-shared-slow-motion). This section covers what players experience and how gameplay stays consistent inside one.

#### What everyone sees
- **Activator:** the whole world slows (NPCs, bullets, vehicles, physics and the other players). The activator moves, aims, fires and reloads at full speed.
- **Everyone else in range:** the world runs slowly on their screens too, **including their own V**. The activator moves at accelerated speed relative to everything.
- **Two activators with the same rating:** both run at full speed, match each other, and both see everyone and everything else slowed.
- **Different ratings resolve proportionally:** the world runs at the deepest slowdown, and each activator moves at `1/sᵢ` relative to it.

| Active Sandevistans | World runs at | Activator A (0.25) | Activator B (0.5) | Non-activators |
|---|---|---|---|---|
| A only | 25 % | 100 % real speed (4× the world) | — | 25 % |
| B only | 50 % | — | 100 % real speed (2× the world) | 50 % |
| A and B | 25 % | 100 % (4× the world) | 50 % real speed (2× the world, half of A) | 25 % |

#### Per-player time scales
Every machine computes the same per-entity rates from the same activation set ([01 §8.2–8.3](01-architecture.md#82-the-rate-function)): world rate `r_f = min(1, minᵢ cᵢ)`, activator `i` at `r_f / cᵢ`, everyone else at `r_f`. Each player's machine applies its own V's rate locally; puppets of other players play their animation at that player's rate. No entity ever needs to run faster than real time.

#### Bullets and hits fired by a fast player
- **Fire events** are stamped with field world time `W`. Other machines replay them on the `W` axis, so they see the activator fire at `1/cᵢ` times the normal rate, exactly as fast as it happened.
- **Bullets and projectiles** are world objects and move at the world rate: the activator's own bullets crawl through the air, identically on every screen. Hitscan hits are resolved at the `W` the shooter saw.
- **Hit validation:** `HitRequest` carries `seenAtW`; the target's owner rewinds to it in `W`. Fire-rate checks use the shooter's relative speed `1/cᵢ`. Melee and Mantis Blade hits work the same way.

#### NPC reactions stay consistent
- Every owner of NPCs in the field runs them under the same dilation at the same `W` (the clock follower keeps machines within milliseconds of world time), so detection timers, reaction delays, aiming, dodges and grenade fuses play out the same no matter which machine owns which NPC.
- NPCs face a target (the activator's puppet) moving `1/cᵢ` times faster than they are: that's vanilla behavior against a Sandevistan user.
- Authority handoffs during a field are allowed; all state times in a transfer are in `W`.

#### The activator's view of other players stays smooth
Other players are really slowed on their own machines, so their snapshot stream is genuine slow motion. Interpolating it on the `W` axis shows them moving smoothly at the reduced rate, with no growing delay and no catch-up afterwards ([01 §8.7](01-architecture.md#87-interpolation-sending-and-lag-compensation-inside-a-field)). Their send rate drops with `r_f` while the activator's stays at 60 Hz, so bandwidth stays flat.

#### Who is affected (scope)
- **Local field (default):** the activator plus anyone whose surroundings overlap with a field member's, transitively. Players elsewhere keep playing at normal speed: nothing they can see is slowed, which matches the rule that players elsewhere are never disturbed. Someone driving into range eases into the slow motion over 300 ms.
- **Global (host setting):** every player in the session is slowed whenever anyone activates, wherever they are, including during story scenes elsewhere.

#### Edge cases
- **Scenes and dialogue inside a field** slow with it: the conversation is happening in the slowed area. Dialogue choice timers count world time.
- **Vehicles in a field** slow with the world. [VERIFY] vanilla prevents activating Sandevistan while driving; if so, the activator never drives at a different rate from their car.
- **Downed players' bleed-out** counts world time (slowed in a field).
- **Camera [COMPROMISE]:** a slowed player's body, movement, weapon handling, aim-down-sights and recoil recovery run at the world rate, but **mouse look stays at real-time speed**. Slowing camera rotation is uncomfortable and can cause motion sickness. The side effect is slightly easier aiming for slowed players against slowed targets. Host setting "Slow camera turn in time fields" (off by default) for groups who want it strict.
- **Audio:** world audio is dilated by the engine on every member's machine, as in vanilla.
- **Menus and loading:** a member in a menu doesn't pause anything (§20); a machine in a loading screen is never a field member.
- **Durations and cooldowns:** the activator's Sandevistan duration and cooldown run in the activator's own time, as in vanilla; the activation's `T_end` follows from that.
- **Kerenzikov:** same mechanism, triggered on dodge/slide while aiming, with a short duration and its own scale.
- **NPC Sandevistan users** speed themselves up individually on their owner's machine; their animation rate is replicated. They don't create a field.

#### Hooks [VERIFY]
- Activation/deactivation path: player state-machine scripts and `TimeDilationHelper` (search the script dump for `TimeDilation`); scale, duration and ease curves per item (TweakDB records of each Sandevistan/Kerenzikov).
- How vanilla applies global dilation and exempts the local V; whether an arbitrary entity (a puppet) can be exempted and given an individual rate ≤ 1 (CET probe on a spawned NPC).
- Reading the engine's simulated world time each frame (for the clock follower) and changing global dilation every frame without hitches.
- Whether time of day dilates.
- All of the above is spike S8.

#### Fallback
- If puppets can't be exempted from global dilation, drive their animation playback rate through an animation-graph rate input (they're network-driven anyway, so only animation speed is at stake).
- If global dilation can't be modulated smoothly per frame, the clock follower corrects only at ease boundaries and lets interpolation absorb the remaining error (up to ~50 ms of world time).

**Rating:** time-field clock and rates Hard; puppet exemption Research (S8); local-scope membership Hard.

### 6.2 Optical camo
- **Logical model:** other players see exactly what an NPC sees: a near-invisible shimmer.
- Puppet gets the same optical-camo visual effect NPC netrunners use when cloaked. On NPC owners, the puppet's camo flag feeds the same stealth checks as the real V's camo (detection suppressed, broken by attacking).
- [VERIFY] how camo is represented for V (status effect record and visibility stat or tag) and which visual effect NPC camo uses (TweakDB/effect resource search in WolvenKit).
- **Rating:** Hard.

### 6.3 Berserk, double jump, Mantis Blades, Gorilla Arms, Projectile Launch System
- **Movement abilities** (double jump, charged jump, Mantis leap, air dash): the player's own movement is authoritative and replicated through `PlayerState` with the ability id in the animation state, so others see the jump/leap exactly.
- **Melee** (Mantis Blades, Gorilla Arms, Monowire): melee hits use the same `HitRequest` path with `attackType = melee` and the melee move id; Gorilla Arms slam and Mantis leap landing use `AreaEffect`.
- **Berserk:** buff state replicated via `PlayerAbility(berserk, on, duration)` for visuals; its slam is an `AreaEffect`; its damage/armor bonuses apply on the activator's own offense and defense calculations.
- **Projectile Launch System:** projectile is a thrower-owned dynamic entity; detonation is an `AreaEffect`.
- **Rating:** Hard each (animation sync for each ability is the main work).

### 6.4 The general rule for grenades, hacks and status effects
Every effect is expressed as either a targeted message to the **owner of the affected entity** or an `AreaEffect` that each machine applies to **what it owns**. Nothing is applied "on the attacker's screen only". That gives one rule for everything: an effect affects whatever it would logically affect, from every perspective.

---

## 7. Enemy scaling

- **Logical model:** more mercs in a fight means a tougher fight. Scaling counts the players actually **in the encounter**, not in the session.
- **Encounter count:** players within 80 m of the NPC when it enters combat, plus anyone who damages it later. When the count rises mid-fight, max health scales up preserving the health percentage. It never scales down mid-fight (so leaving doesn't exploit it).
- **Health:** stat-modifier multipliers on the NPC's max health, applied by the owner.
- **Damage:** applied by the victim's machine in the defense step, multiplying damage from NPC sources by the encounter's damage factor.
- **TweakXL:** `code/assets/Tweaks/Coop/scaling.yaml` defines modifier records per player count (`Coop.Scaling.Health.P2..P4`, `Coop.Scaling.Damage.P2..P4`, plus `Boss` variants). Host-configurable overrides in the session settings replace the multiplier values at runtime by creating modifiers through the stats system.
- Defaults (tunable): 2p health ×1.5, damage ×1.1; 3p ×2.0 / ×1.2; 4p ×2.5 / ×1.3; bosses use separate multipliers.
- [VERIFY] stats system modifier API and that max-health modifiers recompute the pool correctly (CET: add a multiplier to an NPC, read its health pool).
- **Rating:** Easy.

---

## 8. Death and revive

### Logical model
A merc who goes down bleeds out unless someone patches them up. If nobody does, they wake up somewhere a ripperdoc could have taken them. The world doesn't rewind.

### Plan
- **Downed:** a hook on the local V's death converts it to a downed state: health floors at 1, V falls into a wounded pose and can crawl slowly, look around, and use a sidearm only if the host enables "last stand". Vanilla game-over is suppressed. `PlayerDowned(player, bleedOutEnd)` to all.
- **Revive:** any other player can hold the "Revive" interaction on the downed player's puppet (3 s, reviver kneels in a paired animation, both visible to everyone). Interrupted by taking damage. Revived player returns with 30% health. Bounce Back / health items speed it up.
- **Bleed-out:** 60 s default (host setting). Then the downed player chooses (or auto after 10 s) to **respawn** at the nearest safe location: the nearest ripperdoc clinic or V's apartment that isn't inside an active combat zone, with a configurable eddie fee ("patched up by a ripperdoc"). Only that player moves; it's their own respawn.
- **Others are unaffected**; there is no reload anywhere.
- **Host death:** identical. The quest just continues: the fight is still there when the host gets back. In vanilla, V's death never reaches the quest graph except as a reload, so main quests have no "V died" branch to break.

### Quest-critical failure
Quest-scripted failures (escort dies, target escapes) that would show "mission failed" and reload a checkpoint in vanilla:
- **v1 [COMPROMISE]:** the host's machine reloads its last quest checkpoint. Clients are **not reloaded or moved**; they receive `WorldReset` and re-baseline facts and the world-delta log, and encounter entities reset. Their own characters keep everything. This is the least intrusive option available without partial rollback.
- **Research:** encounter-scoped rollback without a reload: restore only the facts written since the quest's last checkpoint and restart that quest phase. Possible only if spike S5 shows phases can be restarted.

### Hooks [VERIFY]
Player death path (`PlayerPuppet` death handling and the death menu controller in the script dump), health stat pool floor (stat pools system), quest game-over node type (find in questphase files, then the native handler).

**Rating:** downed/revive Hard; quest-failure handling Research.

---

## 9. Loot, money, XP, street cred

- **Containers and enemy loot are instanced per player** (as specified). Each machine rolls loot locally when its own V opens a container or loots a body, keyed `(containerNetId, playerId)`. The host records `ContainerOpened` so each player's view stays consistent across sessions (empty for the player who looted it, still full for the others).
  - Note the tension with the core principle: physically, a looted box would be empty for everyone. The spec explicitly asks for instancing, so that's the default; host setting **"Shared loot (first come)"** makes containers physical instead.
- **Dropped items** (a player drops a weapon, an item dropped by a quest) are physical and shared: first pickup wins, arbitrated by the item's owner.
- **Money from containers and enemies:** instanced with the loot.
- **Quest rewards:** the quest's protagonist receives the vanilla reward. Others present receive `RewardGrant` with the same money and XP. Item rewards are copied to others only if not quest-tagged or unique-to-story.
- **Kill XP:** each player within 50 m of the kill or who damaged the NPC in the last 30 s gets full kill XP (`NpcDeath(contributors[])`), granted locally. Skill progression (Headhunter, Netrunner, Shinobi, Solo, Engineer) comes from each player's own actions as in vanilla, credited to the player who did the action.
- **Street cred:** from each player's own activity and from shared quest rewards.
- [VERIFY] when container loot is generated (on first open versus on spawn) and how to roll it locally for a proxy container (loot manager in the script dump; CET probe opening the same container twice).
- **Rating:** Hard.

---

## 10. NCPD heat and wanted level

### Logical model
Cops chase the person who committed the crime and was seen doing it, wherever that person is. Anyone who joins the fight against the cops is committing a crime too.

### Plan
- Each machine's prevention (police) system runs **for its own V only**.
- Crimes committed by a **remote** player are witnessed by NPCs on the witness owner's machine; a hook on crime reporting turns "perpetrator is a puppet" into `CrimeWitnessed(perpetrator, crimeType, witnesses)` sent to the perpetrator's machine, which injects it into its own prevention system.
- Police units for a wanted player are spawned **by the wanted player's machine** around that player and pinned to it; other machines see proxies and can fight them. Attacking police is a crime for the attacker.
- `HeatUpdate(player, stage)` keeps party HUDs and map icons current.
- Two players wanted in the same area get two responses. That's logical (twice the crime), with a cap: if responses overlap, the second machine's spawner tops up to a shared limit instead of doubling.

### Hooks [VERIFY]
2.0+ prevention system (scriptable system `PreventionSystem` in the script dump): crime reporting entry point, heat stage changes, and how its spawner picks positions relative to the local player. Spike S10.

**Rating:** Research.

---

## 11. Vehicles

- **Vehicle sync (built in M0):** the driver's machine simulates the vehicle; others receive 30 Hz snapshots (transform, velocity, steering, wheel spin, suspension, lights, horn, damage state) and run the vehicle as a kinematic proxy with matching visuals. Remote passengers are puppets mounted to seat slots. [VERIFY] how to mount a non-player entity into a vehicle seat slot and play the seated animations (vehicle mounting system in NativeDB; CET probe seating an NPC).
- **Ownership:** a driven vehicle is owned by the driver's machine. Parked vehicles belong to the cell owner (M1; until cells exist, a parked vehicle stays with whoever simulated it last).
- **Rules as built (M0b, `HostService`):** the host arbitrates seats; taking the driver seat moves simulation to the driver's machine and bumps the vehicle's epoch, and snapshots carrying an old epoch are dropped. A player sits in one seat at a time. Only the player who summoned a vehicle can dismiss it, and not while someone else is inside. When the summoner leaves the session, their vehicles leave with them; when a borrowing driver leaves, the vehicle goes back to the summoner's machine. Vehicle ids are `(spawning peer << 24) | counter`, so machines never collide and the host can reject forged spawns. Up to 16 vehicles per player.
- **Owned vehicle calls:** each player summons from **their own** garage (from their character profile), spawned by their machine and replicated. Calls never touch another player's garage.
- **Vehicle combat (2.0):** hits on vehicles and from vehicle weapons use `HitRequest`/`AreaEffect`.
- **Delamain, AVs, scripted chases:** quest vehicles owned by the machine running the quest. Free seats are real seats for other players. Chases with mounted shooting: passengers shoot from their own machine through the normal hit path.
- **Rating:** owned-vehicle sync Hard; scripted vehicles Research.

---

## 12. Fast travel

- **Individual:** only the traveler moves. Locally it's the vanilla fast-travel load; the session stays connected (net thread keeps running; roster shows `Loading`).
- Others see the traveler's puppet disappear at the terminal with a short fade. [COMPROMISE, minor] Vanilla fast travel is instantaneous; there's no physical in-between to show.
- After the load, the traveler's interest region changes and they receive fresh baselines for their new cells.
- [VERIFY] that fast travel doesn't trigger anything session-wide (time skip, global resets) in the fast-travel system.
- **Rating:** Hard (load-time robustness is the work).

---

## 13. Sleeping, waiting, time skip

- **Logical model:** there is one clock. If one person waits six hours, everyone's world is six hours later, so it has to be agreed.
- **Vote:** a player uses a bed or the wait menu → `SkipRequest(targetTime)`. Everyone gets a non-blocking notice "Filip wants to skip to 22:00 (Accept / Decline)". The skip commits when every player accepts; a player who doesn't answer within 20 s while idle (not in combat, not in a scene) counts as accept. Host setting: "vote" (default), "host only", "anyone".
- **Commit:** `SkipCommit(newTime)`; each machine advances its clock. Nobody moves. The requester sees the vanilla sleep/wait presentation; others see a 2-second time-lapse of the sky and lighting instead of a fade.
- **Quest-forced skips** (story scenes that jump to the next day) run on the host and apply to everyone with a notice "Time passes (story)".
- [VERIFY] the time-skip path in the time system (search the dump for the wait/sleep UI controller and what it calls) and how NPC schedules react to a clock jump without a fade.
- **Rating:** Hard.

---

## 14. Phone, messages, gigs, fixers

- The host's phone drives quests: messages, calls and fixer offers arrive on the host's machine as in vanilla.
- Clients get a **party feed**: "Host got a message from Regina" with the text preview (host setting: preview on/off), and mirrored quest notifications.
- **Gigs:** leaseable gigs (§1c) show to every player on the map as party jobs. Any player can take one by going there.
- Clients' own phones show their own contacts list read-only and the party feed; story contacts can't be called by clients.
- [VERIFY] message and call delivery points (journal contact/message entries, phone system) in the script dump.
- **Rating:** Hard.

---

## 15. World streaming and persistence

- Each machine streams around its own V (vanilla behavior).
- Global state replicated to everyone: facts, journal, time, weather, heat records, world-delta log ([01 §6.3](01-architecture.md#63-world-delta-log-wdl)).
- When a machine streams an area in, WDL entries for that area apply before the entities become visible, so a door one player unlocked or a named NPC another player killed is in the same state when someone arrives.
- **Quest content away from the host [COMPROMISE]:** content spawned by the host's quest nodes exists only once the host's machine streams that area. A client who arrives at a quest location long before the host finds it empty; the quest content appears as the host approaches. Research option: have the host's machine stream an extra quest-relevant bubble around remote players (spike S9; memory and CPU cost unknown). Leasing (§1c) removes this for self-contained content.
- **Rating:** Research.

---

## 16. Special content

| Content | Logical model | Plan | Rating |
|---|---|---|---|
| **Braindances** | The protagonist wears a wreath and is in the recording | Protagonist's body sits/stands with the wreath, visible to all. Watching: Discord/Steam screen share in v1. Research: co-viewing by playing the same BD on another machine in viewer mode, synced to the protagonist's timeline | Hard (v1) / Research |
| **Cyberspace** (Alt, Songbird, Mikoshi) | V is jacked in physically | Body in the chair, visible. Others guard it (enemies can still arrive). Watching via screen share | Hard / Research |
| **Johnny flashbacks** | It's a memory | Same as cyberspace | Hard |
| **Dogtown** | Part of the open world | Same as the rest of the map, including PL airdrops (treated as dropped items/containers) | Same as base |
| **Phantom Liberty main jobs** | Host's story | Same as §1–§2; Songbird calls follow the holocall rule | Research (scene replay) |
| **Arasaka Tower / point of no return** | The host's V commits to the end | Missions that take place in the normal world (assaults, Nomad tank ride, Rogue's raid) are playable together with real seats and real fights. Sequences in isolated spaces (Mikoshi, credits, epilogue) are host-plays, others spectate via screen share | Research |
| **Endings** | The host's story ends | After the epilogue, vanilla loads the pre-PONR save. Clients get `WorldReset` and stay where they are [COMPROMISE: the game's own design] | Research |

**Integrated spectating** (rendering the host's view inside the client's game) would require the client's machine to render a different world state than the one its V stands in; it isn't realistic in the engine. If wanted later, the practical path is a hardware-encoded video stream from the host (NVENC/AMF) shown in a client overlay: 3–6 Mbit/s of host upload per watcher.

---

## 17. Save system

### Rules
1. **Before any session**, every machine copies its whole save folder to `coop/backups/<timestamp>/` (keep last 10). [VERIFY] save location (`%USERPROFILE%\Saved Games\CD Projekt Red\Cyberpunk 2077\`).
2. **Host:** saves normally; the world is the host's save plus the session sidecar `coop/worlds/<worldId>/` (WDL, opened-container index, client roster, lease state), written atomically whenever the host saves.
3. **Clients never save during a session.** Manual saves, quicksaves and autosaves are intercepted and replaced by a **character profile snapshot**: `coop/profiles/<worldId>/<clientId>/character.json` (every 60 s, on level-up, on purchase, on session end), atomic write (temp → flush → rename), checksummed, 3 rotating backups. Our plugin writes these from C++; RedFileSystem is used for the redscript-side read of settings.
4. **Nobody ever writes into another player's save.** The host never stores a client's character; a client never writes the host's world.

### Join modes (how a client's game gets the host's world)
- **Mode A — overlay (M0–M2):** the client loads its **own** save, then receives the baseline overlay (facts, journal, WDL, time, weather) and freezes its quest execution. Fast to build. Weakness: world state that isn't fact- or WDL-driven (quest-controlled world layers, persistent states the overlay doesn't cover) can differ between the client's world and the host's. Acceptable before quests matter (M3).
- **Mode C — composite (target, M3+):** the client loads a world built from the **host's latest save** with the **client's character data substituted**, then applies the overlay deltas since that save. Every world state matches the host's by construction, and the client's own V (body, gender, voice, appearance, stats) stays theirs. Requires a map of which save nodes are "character" and which are "world", and either offline stitching or substitution at load time. Spike S7. The host's save (up to 15 MB) transfers on lane L3 once per session and is cached per world.

### Return to own world
1. Session ends or the client leaves → final snapshot written, `pending-return` marker set.
2. Client's game loads **the save it joined from** (path recorded at join).
3. The snapshot is applied at runtime: level/XP, attributes, perks and skills, inventory diff (exact item ids, seeds and attached mods), equipped cyberware, money, street cred, crafting specs, vehicles. Quest-tagged items are stripped.
4. Matched leaseable completions are transplanted (§1).
5. The result is written to a **new** save slot ("Co-op return – <host> – <date>"). Existing slots are never overwritten.
6. Marker cleared. If the game crashes at any step, the marker triggers a recovery prompt on next launch.

[VERIFY] runtime APIs for setting perks and skills in 2.x (`PlayerDevelopmentSystem` requests), and that item ids with random seeds and mods round-trip through serialization (spike S7).

**Rating:** save lock + profile + return Hard; composite join Research.

---

## 18. UI

- **Main menu "Co-op"** entry (new ink controllers built with Codeware): Join (direct IP / join code / Steam invite), recent sessions, profile management.
- **Hosting:** in-game pause menu "Open to co-op" (like opening a LAN world): password, max players, scaling, time-field scope (local/global), slow camera turn in time fields, loot mode, call sharing, time-skip rule, respawn fee, friendly fire (off by default).
- **Party HUD:** player list with health/armor, downed timer, heat level; on-screen distance markers and off-screen indicators (mappins); map icons.
- **Choice mirror panel** (§2), **party feed** (§14), **time-skip vote** toast (§13).
- **Client-local settings** via Mod Settings (interface scale, marker style, feed verbosity).
- **Rating:** Hard (lots of ink work, low technical risk).

---

## 19. Mod compatibility

- **Manifest** computed at startup (cached by path + size + mtime): game exe version and SHA-256, RED4ext and every RED4ext plugin DLL, redscript sources (`r6/scripts/**`), tweaks (`r6/tweaks/**`), archives (`archive/pc/mod/**`) and ArchiveXL `.xl` files, CET mod folders, and our own version.
- **Classes:** *Gameplay* (scripts, tweaks, RED4ext plugins, CET mods, archives containing non-cosmetic resources): must match exactly. *Cosmetic* (archives containing only meshes, textures and materials): may differ; others see vanilla fallbacks. *Client-local* (an allowlist of UI/QoL mods): ignored.
- **Join check:** `Hello` carries the manifest hash; on mismatch the host sends the full manifest and the client shows a readable diff ("missing on your side: X; different version: Y").
- **Rating:** Easy.

---

## 20. Pause, menus and presentation slow-motion

### Logical model
The world doesn't stop because one person opens their inventory, and it doesn't slow down because someone is looking through their scanner. Only cyberware that actually bends time (§6.1) slows the world.

### Plan
- **Alone in your field** (no other player's surroundings overlap yours, the same test as time-field membership): vanilla behavior. Menus pause your game, the scanner slows time, photo mode works. Nobody can observe your area, so pausing it is consistent; the paused time becomes clock offset exactly like a time field's debt ([01 §8.6](01-architecture.md#86-clock-debt)). The network keeps running underneath, and global events queue until you resume.
- **Not alone:** menus (inventory, map, journal, perks, crafting, phone, pause menu) open **without pausing**. Your V stays in the world, as vulnerable as in Seamless Co-op; others see a small "in menu" marker over your puppet. Scanner and quickhack targeting work without the slowdown. Cinematic finisher slow-motion is skipped.
- **Someone comes into range while you're paused:** your game resumes behind the open menu, with the notice "Another player is nearby — game resumed." Their shared view of your area only starts once your world is running again.
- **Photo mode:** available when alone. [COMPROMISE, minor] When not alone it's unavailable, because vanilla photo mode pauses the world. [VERIFY] whether photo mode can run with the world live (free camera without pause); if so, allow that instead.
- **Scripted slow-motion in story moments:** treated as a time field with the protagonist as the activator and the script's scale, so everyone nearby sees the same moment.

### Hooks [VERIFY]
Pause requests from menus (time system pause/dilation reasons, UI system menu-open events in the script dump); scanner slowdown (the dilation reason used by the scanner/quickhack HUD); photo mode toggle; finisher camera slow-motion; scripted dilation nodes in questphase/scene files.

**Rating:** Hard.
