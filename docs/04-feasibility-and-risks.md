# 04 — Feasibility and risk list

Deliverable 2. Ratings:

- **Easy** — known technique, engine surface already exposed to script or RED4ext; mostly implementation time.
- **Hard** — known technique, but large, fiddly, or depends on engine behavior we expect to work; moderate risk.
- **Research** — depends on an engine behavior nobody has (publicly) demonstrated; could turn out impossible, forcing a fallback. Each Research item names the spike that settles it.

## 1. Bottom line

- **Feasible with high confidence:** the core of M0–M1. Two to four players in one world, seeing and fighting alongside each other, distributed NPC ownership, downed/revive, scaling, loot, vehicles, fast travel, version and mod checks. Everything is built from scratch, so M0 is a real milestone of its own (plugin shell, transport, puppets, appearance, locomotion, vehicles).
- **Shared Sandevistan slow motion is consistent and feasible** with the time-field design ([01 §8](01-architecture.md#8-time-fields-shared-slow-motion)). It uses the engine's own global time dilation plus a per-entity exemption; nothing ever needs to run faster than real time. The remaining unknown is whether a puppet can be exempted from global dilation (S8), and there's a fallback.
- **Feasible with real risk:** host-driven main quests where clients are present (facts, journal, observer scenes). This hinges on two engine questions: can a client's quest execution be frozen while facts stay live (S5), and can a scene be replayed on a client with another entity in V's role (S6).
- **The spec's hardest promises:**
  1. *"Everything playable, anywhere, independently."* Quest content only exists where the machine running that quest has streamed. For self-contained content, quest leasing (S5) can make it true. For the main story, it's true only near the host. [COMPROMISE]
  2. *"Clients join with their own character into the host's world, with an identical world."* Needs a character-vs-world split of the save format (S7). Until then, join mode A has small world mismatches.
- **Scale:** this is a multi-year hobby project or a roughly 1.5–2 year full-time one for an experienced reverse engineer. Every milestone is designed to be a playable, releasable state.

## 2. Ratings by system

### Foundation (all built from scratch)

| System | Rating | Why | Depends on |
|---|---|---|---|
| RED4ext plugin shell, RedLib-registered `CoopSystem`, redscript natives, ImGui overlay | **Easy** | Standard RED4ext/RedLib patterns on 2.31 | S1 |
| Transport, handshake, clock sync, password | **Easy** | GNS provides the hard parts | — |
| In-process HostService (arbiter + relay) | **Hard** | Pure engineering; no engine calls | — |
| Remote-player puppet spawn (Codeware) | **Easy** | Public `DynamicEntitySystem` API | S1 |
| Puppet appearance from the player's character-customization state | **Hard** | Applying CC state to a non-player entity | S1 |
| Locomotion animation sync (idle, walk, run, sprint, crouch, slide, jump) | **Hard** | Driving animation-graph inputs on a proxy | S3 |
| Player combat animation sync (aim, fire, reload, cover, melee, cyberware moves) | **Hard** | Same mechanism, many more states | S3 |
| Vehicle sync and seat mounting | **Hard** | Kinematic vehicle proxies, mounting puppets into seats | S1 |
| Steam relay + invites | **Research** | Whether app 1091500 permits relay P2P from inside the game; interface version of the bundled `steam_api64.dll`; coexistence with statically linked open-source GNS. Deferred: testing needs two accounts that each own the game | S11 |
| Two game instances on one PC (dev instance mode) | **Research** | Single-instance guard, save/settings separation, background simulation; depends on the store allowing a second process | S13 |
| SimClient / SimHost headless test tools | **Hard** | Second implementation of the client and host protocol sides, without the engine | — |
| GOG Galaxy transport | **Research** | Whether a mod can use Galaxy networking with the game's credentials | — (deferred) |
| Version pinning, address-library hook self-test | **Easy** | RED4ext API v1 on 2.31 | — |
| Snapshot/delta/interpolation core | **Hard** | Well understood; volume of work | — |
| Remote players as AI targets | **Research** | Scripts special-case the real V widely | S2 |
| Distributed ownership + handoff | **Research** | NPC "proxy mode", deterministic entity identity, despawn pinning | S3, S4, S9 |
| Crowd and traffic in shared cells | **Research** | Suppressing ambient spawners per area; swap is a compromise anyway | S3 |

### Systems from the spec

| System | Rating | Why | Depends on |
|---|---|---|---|
| Fact replication | **Easy** | Single native setter to hook | — |
| Client quest follower mode | **Research** | Freezing graph execution without breaking fact-driven world state | S5 |
| Journal / objectives / map pins mirror | **Hard** | Many entry types | S5 |
| Remote quest triggers | **Hard** | Must identify condition node evaluators | S5 |
| Quest leasing (gigs, hustles, side jobs) | **Research** | Per-quest execution on a client | S5 |
| Client progress rule (character kept, world not) | **Hard** | Runtime character application | S7 |
| Matched-progress transplant for leaseable quests | **Hard** | Offline dependency table + fact transplant | S5, S7 |
| Scenes: observer replay | **Research** | Rebinding the player performer | S6 |
| Scenes: animation-hint fallback | **Hard** | Lower fidelity, but uses known pieces | S3 |
| Dialogue choice mirroring | **Hard** | Blackboard data capture + UI | — |
| Locked-movement, car-ride scenes | **Hard** | Falls out of observer mode + seat sync | S6 |
| Holocalls (one-sided audio, optional party line) | **Hard** | VO routing | S6 |
| Romance privacy volumes | **Hard** | Data table + door locks + start gating | S6 |
| Johnny: each player sees only their own | **Hard** | Local-only Johnny; observer scenes bind the Johnny role to the observer's own Johnny; dedup with ambient Johnny; never replicate the takeover swap | S6, S5 |
| Hit registration, damage, status effects | **Hard** | Injecting remote hits into the damage pipeline | S12 |
| Enemy aggro split | **Research** | Same as AI targeting of puppets | S2 |
| Stealth / detection replication | **Research** | Sense and detection internals | S2 |
| Takedowns, grapples, carried bodies | **Research** | Paired animations across machines + fast authority grant | S3 |
| Quickhacks (damage, status) | **Hard** | Same pipeline as hits | S12 |
| Quickhacks altering AI or spreading | **Research** | Owner-side execution with a remote instigator | S2, S12 |
| Breach protocol | **Hard** | Local minigame, owner-side daemons | — |
| Time fields: shared clock, rate function, clock follower | **Hard** | Uses vanilla global dilation; the follower controller and stamping everything in field time are engineering | S8 |
| Time fields: activator and puppet exemption with individual rate ≤ 1 | **Research** | Exempting arbitrary entities from global dilation | S8 |
| Time fields: local-scope membership, ramps, clock offsets | **Hard** | Host-side set logic plus stamp translation | — |
| Pause, menus and presentation slow-motion rules | **Hard** | Intercepting every pause/dilation source | S8 |
| Optical camo | **Hard** | Visual effect + stealth flag on puppet | S2 |
| Berserk, double jump, Mantis Blades, Gorilla Arms, PLS | **Hard** | Per-ability animation + AreaEffect | S3 |
| Grenades and area effects (general rule) | **Hard** | One `AreaEffect` path | S12 |
| Enemy scaling | **Easy** | Stat modifiers + TweakXL records | — |
| Downed / revive / respawn | **Hard** | Custom state, paired revive animation | S3 |
| Quest-critical failure handling | **Research** (v1 compromise is Hard) | Phase restart without reload | S5 |
| Instanced loot, money, XP, street cred | **Hard** | Local loot rolls on proxy containers | — |
| NCPD heat per player | **Research** | Prevention system is built around the local V | S10 |
| Owned vehicles, summons, vehicle combat | **Hard** | Ownership on top of vehicle sync | S1 |
| Delamain, AVs, scripted chases | **Research** | Quest-driven vehicle behavior on observers | S5, S6 |
| Fast travel (individual) | **Hard** | Keep session alive across local loads | — |
| Time skip vote | **Hard** | Clock jump without fade on other machines | — |
| Phone / messages / fixers mirror | **Hard** | Many UI touchpoints | — |
| World streaming + world-delta log | **Research** | Persistent-state restore hook | S9 |
| Quest content away from the host | **Research** | Extra streaming anchors on the host | S9 |
| Braindances, cyberspace, flashbacks (body visible + screen share) | **Hard** | Mostly falls out of scene work | S6 |
| Integrated spectating | **Research** (not planned) | Rendering a second world state | — |
| Phantom Liberty main jobs, Dogtown | **Research** / same as base | Scene replay; Dogtown is open world | S6 |
| Arasaka Tower, PONR, endings | **Research** | Heavily scripted; isolated spaces; post-ending reload | S5, S6 |
| Save lock, profile snapshots, backups, return | **Hard** | Intercepting all save paths; runtime character apply | S7 |
| Composite join (host world + client character) | **Research** | Save node map; gender/body swap | S7 |
| Lobby, party HUD, settings UI | **Hard** | Volume of ink work; low risk | — |
| Mod manifest hash and diff | **Easy** | File hashing | — |
| Desync detection and logging tool | **Hard** | State hashing + netlog viewer | — |

### 2.1 Totals

66 rated items: 7 Easy, 36 Hard, 23 Research.

## 3. Reverse-engineering spikes (do these first)

Ordered by how much of the design they decide. Each is a 1–5 day experiment with a written result before dependent code is written. **Results so far: [08-spike-results.md](08-spike-results.md)** (S1, S2, S3, S8 run; S1 vehicles confirmed; S2 failed as first tried).

| # | Question | How to check | Decides |
|---|---|---|---|
| **S1** | Bring-up on 2.31: can our plugin register a ticking game system and redscript natives, spawn an NPC puppet through `DynamicEntitySystem`, apply a player's character-customization state and equipment to it, and seat it in a vehicle? | Minimal plugin + CET probes; NativeDB for the character customization system and vehicle mounting | M0 foundation |
| **S13** | Can two instances of your one copy run on one PC? What is the single-instance guard and is it checked before RED4ext plugins load? Which API locates saves and settings? Does an unfocused instance keep simulating, and does gamepad input reach it? Does your store allow a second concurrent process? | Launch twice and observe; Process Explorer for named mutexes/events; API Monitor or a logging hook on known-folder lookups; test focus and gamepad behavior. No DRM workarounds: if the store refuses, fall back to SimClient/SimHost | Whether two-instance testing works ([05-local-testing.md](05-local-testing.md)) |
| **S2** | Will hostile NPCs detect, target, shoot and melee an NPC-based puppet with attitude group `player`? Do bullets register on it? | CET: spawn a puppet next to a gang, set its attitude group, observe; log target-tracking queries. Script audit of `GetPlayer`/`IsPlayer` call sites (count and classify) | Puppet design; aggro, stealth, camo, AI-altering hacks |
| **S3** | Can an arbitrary human NPC run in "proxy mode": AI off, transform and animation driven externally, still physically present? Which animation-feature inputs drive locomotion and combat? Can ambient crowd/traffic be suppressed in an area? | RTTI dump of AI and animation components; CET: disable AI on an NPC, set position each frame, push animation features; observe | Distributed ownership; animation sync; crowd plan |
| **S8** | How do Sandevistan and Kerenzikov dilate time and exempt V? Can an arbitrary entity be exempted from global dilation and given its own rate ≤ 1? Can global dilation change every frame without hitches? Can the simulated world time be read per frame? Does time of day dilate? | Script dump search for time-dilation helpers and the Sandevistan state machine; CET: exempt a spawned NPC, set its individual rate, vary global dilation per frame | Time fields; pause and scanner rules |
| **S4** | Are entity ids of placed and community entities identical on two machines at the same spot? | Two instances (same PC is fine), same location, log `EntityID`, persistent ids, spawner info; diff | Entity identity classes |
| **S5** | Can the quest system's graph execution be frozen on a client while facts and journal stay live? Can one quest be unfrozen? Can a phase be restarted? Which facts gate Johnny's presence? | Ghidra on the quest system update path; CET experiments setting facts with execution frozen; test on a gig | Follower mode, leasing, quest-failure handling, Johnny gating — the core of M3 |
| **S6** | Can a scene resource be started on a client with another entity in the player performer role (and the client's own Johnny in the Johnny role), without affecting the local V? How is V's VO variant chosen? | Ghidra on scene start and actor resolution; WolvenKit inspection of `.scene` actor specs; CET trigger of a simple scene | Scene plan (replay vs animation hints); Johnny in scenes |
| **S7** | Which save nodes are character and which are world? Can body gender/voice change after load? Do item ids with seeds and mods round-trip? | Save-diff tool: saves before/after character-only and world-only actions, diff node trees; check whether the player entity template differs by gender | Join mode C; return-to-world reliability |
| **S9** | Can an entity be kept alive outside the local player's streaming range? Can the engine stream extra areas around a non-player anchor? How are persistent states restored on stream-in? | RTTI/Ghidra on streaming system; CET experiments with dynamic entities far from V | Handoff "no vanish"; quest content away from host; WDL apply hook |
| **S10** | How does the 2.x prevention system report crimes and choose police spawn positions? Can a crime be injected for the local V from outside? | Script dump (`PreventionSystem` and related); CET injection | Per-player heat design |
| **S11** | Do Steam networking sockets with relay P2P work under app 1091500 from inside the game? Which interface version does the bundled `steam_api64.dll` export? Can it coexist with open-source GNS in one process? | Small RED4ext test plugin calling the flat API between two Steam accounts. **Deferred:** needs a second tester with their own copy | Steam backend in M5 |
| **S12** | Can a hit with a remote instigator (a puppet) be applied to an NPC through the vanilla pipeline? Can a hit from a remote NPC be applied to the local V with correct mitigation? | Script dump of the hit pipeline; CET: construct and queue hit events | Combat, quickhacks, area effects |

Suggested order for the first month: S13 and S1 together (they decide the whole test setup and the M0 foundation), then S2 → S3 → S8 → S4 (these unblock M0–M1), then S5, S6, S7 in parallel with M1 work, since they decide whether M3 is achievable as designed. S11 waits for a second tester.

## 4. Unknowns by engine area (consolidated [VERIFY] list)

| Area | Unknown | Method |
|---|---|---|
| Game system registration | Tick phases available to a RedLib/RED4ext game system on 2.31 | Spike S1 |
| Threading | Which engine calls are safe off the main thread | Assume none until profiled |
| Character customization | Read local V's CC state; apply to a non-player entity | NativeDB, CET (S1) |
| Vehicles | Mount a non-player entity into a seat slot; kinematic proxy driving | NativeDB, CET (S1) |
| Quest system | Graph update entry point; per-quest pause; phase restart; game-over node handler; Johnny gating facts | Ghidra + RTTI, WolvenKit questphase inspection |
| Journal | Entry-state change API, tracking, counters | NativeDB, script dump |
| Scenes | Performer binding (player and Johnny roles); VO variant; observer mode; choice hub data | Ghidra, WolvenKit `.scene` |
| AI | Disable/enable AI; set combat target and last-known position; squad membership | RTTI dump, CET |
| Animation | Animation-feature structs for locomotion/combat; playback rate | RTTI dump, CET |
| Senses / attitude | Target eligibility of non-player puppets | CET, script audit |
| Damage | Pipeline stages; injecting remote hits | Script dump, CET |
| Stats | Modifier API; max-health recompute | CET |
| Time | Global dilation per frame; per-entity exemption and individual rate; simulated-time readout; time-of-day dilation; time-skip path; pause sources | Script dump, CET (S8) |
| Streaming / persistency | Despawn pinning; extra anchors; persistent-state restore order | Ghidra, CET |
| Prevention | Crime report entry; spawn positioning | Script dump |
| Loot | Generation timing; local roll for proxy containers | Script dump, CET |
| Saves | Save paths; save request interception (manual, quick, auto); node structure | Ghidra, save-diff tool |
| Character | Runtime perk/skill/attribute setting; gender/body swap | Script dump, CET |
| Johnny | Records, appearances, ambient spawner, takeover swap system | TweakDB dump, WolvenKit, script dump |
| Steam | Relay availability; interface versions; callback pumping | Test plugin |

## 5. Legal and project risks

| Risk | Severity | Mitigation |
|---|---|---|
| Clean-room integrity | Medium | No code from other Cyberpunk 2077 multiplayer projects is used, studied for implementation, or linked. All sync, networking and replication code is original. Contributors declare they haven't copied code from such projects; dependencies are limited to the permissively licensed libraries in [01 §1.1](01-architecture.md#11-dependencies) |
| Project license | Low | MIT recommended (compatible with every dependency; allows GitHub and Nexus distribution) |
| CDPR fan content and modding guidelines | Low if followed | Non-commercial, no game files or assets redistributed, no paywalls, clear "not affiliated with CD PROJEKT RED" notice; players must own the game (the mod runs only inside a legitimately launched game and adds no DRM bypass) |
| Steamworks terms | Low | The Steam backend uses only the game's own `steam_api64.dll` at runtime; no Steamworks SDK files are committed or shipped |
| Transferring the host's save to clients (join mode C) | Low | A save is player-generated state, not CDPR assets; transferred only between session members, cached locally, never published |
| Game patches after 2.31 | Low–Medium | CDPR appears to be done with feature patches; address library and pattern scanning, startup self-test, hard version pin |
| Scope | High | Milestones are designed so each one is a playable, releasable state (M1 alone is "co-op combat sandbox in the story world") |

## 6. What would change the plan

- **S5 fails** (no client follower mode): fall back to host-only quest execution with clients' quest systems mirroring facts only, suppressing duplicate local quest effects case by case. M3 becomes much harder; leasing is dropped.
- **S6 fails** (no scene replay): animation-hint fallback for all scenes; lower fidelity in cutscenes for observers. Johnny in observed scenes is then shown by spawning the observer's own Johnny at the scene's Johnny performer position and replaying his animation hints.
- **S2 fails** (puppets can't be real AI targets): owner-side forced target overrides; NPC behavior toward remote players is cruder.
- **S7 fails** (no character/world split): join mode A permanently, with a curated list of world states transferred explicitly.
- **S8 partly fails** (no per-entity exemption): puppets' animation rate is driven through an animation-graph input instead; if global dilation can't be changed per frame, the clock follower corrects only at ease boundaries.
