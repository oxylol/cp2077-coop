# 09 — Development handoff: what you need to keep going

State at version **0.6.0** (2026-10-08). Read this first; the other documents go deeper.

## 1. What this is

A Seamless-Co-op-style mod for Cyberpunk 2077 (base game + Phantom Liberty, **patch 2.31 only**) for 2–4 players, an enhancement of [CyberpunkMP](https://github.com/tiltedphoques/CyberpunkMP) (Tilted Phoques SRL): the host's save owns the world, every client keeps their own character, no tether or teleports, one logical world. Design: [01](01-architecture.md) (architecture), [02](02-systems.md) (every game system), [03](03-network-protocol.md) (messages), [04](04-feasibility-and-risks.md) (ratings and spikes), [06](06-roadmap.md) (milestones M0–M5).

## 2. Ground rules (not negotiable)

- **Legal:** every player owns the game. No CD PROJEKT RED assets or game files in the repository or the package; follow CDPR's fan content and modding guidelines. Never bypass DRM or account checks. The `probe-*.txt` dumps and `anim-record.txt` / `anim-functions.txt` / `anim-graphs-*.txt` from the game are RTTI dumps: keep them out of the repository (`.gitignore` does).
- **Built on CyberpunkMP (since 0.6; the owner's decision, replacing the clean-room rule):** where CyberpunkMP (Tilted Phoques SRL) already solved something, use its solution and improve on it ([01 §1](01-architecture.md#1-foundation-cyberpunkmp)). Its license applies to the whole project ([LICENSE.md](../LICENSE.md)): keep the repository public, credit Tilted Phoques SRL (a header in every file with ported code, and a row in [NOTICE.md](../NOTICE.md)), and **never upload the mod to a modding platform** (GitHub or the authors' own site only). CyberpunkMP's code is for patch 2.2: check every address, offset and virtual-table slot taken from it at runtime, and switch the step off when it doesn't fit, as `src/plugin/Looks.cpp` does. A local checkout for reading: `git clone --depth 1 https://github.com/tiltedphoques/CyberpunkMP` (client code in `code/client`, scripts in `code/assets/redscript`, records in `code/assets/Tweaks`).
- **Saves are never at risk:** back up `%USERPROFILE%\Saved Games\CD Projekt Red\Cyberpunk 2077` before co-op testing; never save during experiments; with two games running, never save manually (they share the save folder).
- **Platform:** RED4ext API v1 on 2.31 (supported; no need to re-check). Requirements for players: RED4ext, redscript, Codeware, TweakXL; CET only for the dev panel.
- **How we work:** compromises are written down where they're made; engine assumptions are marked [VERIFY] until a spike confirms them. New engine calls are tried in the CET dev panel first (wrapped in `pcall`, with a log line written *before* each risky call, so a crash names its cause), and only confirmed calls go into the redscript bridge: one wrong call there stops every script from compiling.
- **User decisions so far:** Sandevistan/Kerenzikov slow everyone nearby (activator at full speed, overlaps proportional, local scope by default); each player sees their own Johnny; loot instanced per player by default; CyberpunkMP's license (was MIT; the project is an enhancement of CyberpunkMP since 0.6); ask before large code changes; prefer hooking the game's systems over reimplementing them. Test setup: one Steam copy, one PC with 16 GB RAM and 12 GB VRAM.

## 3. Status

| Area | State |
|---|---|
| Networking (portable C++) | Done for M0/M1 scope: GameNetworkingSockets transport with lanes and impairment, handshake with password and version checks, clock sync, 30 Hz player state with interpolation, network thread (`SessionRunner`) with the game touched only on the main thread |
| Host | Roster, relay, seat arbitration and ownership handoff for vehicles, time-field validation and proximity groups (150 m), late-joiner snapshots |
| In the game (tested) | Plugin loads on 2.31; host/join from the dev panel; fake players (`coop-sim`) and a second game join; **slow motion applied in the game** (global dilation, V exempt while activating, puppets at their players' rates, normal time when a session ends); up to 0.4.1, puppets were plain NPCs walked by AI move commands |
| In the game (0.5, partly tested) | **Direct-drive puppets:** a third-person V body, AI and movement component switched off by the plugin, placed every frame at the interpolated pose (Codeware `SetWorldTransform`; works, rounds G–H), animated with the remote V's captured animation inputs plus motion values from the pose; per-puppet automatic fallback to AI walking. Capture works (round H). The player bodies (`TPP_Player`) animate with the inputs but show only a neck; the lookalike looks like V but doesn't animate (round I); an added impostor doesn't change the neck (round J). Dressed in V's slot items (dev panel) and fed the player graph's walking feature (`playerLocomotion`, built from the motion values) it runs with correct legs (round L), but it is a first-person woman's body: no head, broken torso, no face or hair. Switching it to third person changed nothing (round M). The lookalike (V's whole look) has a scene graph that doesn't walk, even with V's animation sets lent (round N). Round O: the male player body spawned by template path crashed. **0.6 (from CyberpunkMP):** player bodies are spawned through Character records (`Cp2077Coop.Character.PlayerBody_Male/_Female`), and every puppet (and the mirror) gets its player's look: third-person flag, items, character customization (`src/plugin/Looks.cpp`); round P ([07](07-testing-guide.md) T4m) checks it |
| Headless only so far | Vehicle sync (spawn, seats, handoff, late joiners) between `coop-sim` processes; time fields between bots |
| Not started | Combat replication, NPC ownership, quests/scenes/facts (M2–M3), saves and join modes, UI |
| Tests | 65 (`xmake run coop-tests`); no AddressSanitizer/UBSan errors; ThreadSanitizer clean for our code (§6) |

Compromises in place: a player who sends no look (a coop-sim bot) gets the local V's; puppets are `PlayerPuppet` bodies (never controlled, never registered as players) [VERIFY nothing treats them as a player]; the look's engine addresses and layouts come from CyberpunkMP's patch-2.2 code, checked at runtime; no body both looks like V and walks yet in the game (round P); a puppet that falls back to AI walking lags and doesn't jump or climb; a hooked call's arguments are evaluated twice; a plain-NPC puppet (drive=ai) joins fights on its own (S2d); slow-motion eases are applied as steps; the game's own Sandevistan isn't connected to the session yet; two games share saves.

## 4. How the pieces fit

```
game main thread                                   network thread (SessionRunner, 4 ms)
CoopSystem (RED4ext game system, ticks each frame)  ClientSession  <->  HostService (host only, in-process)
  -> SessionRunner::Pump()  ---- queued events -->        |
       -> RedGameAdapter (IGameAdapter)              GnsTransport (GameNetworkingSockets)
            -> CoopBridge.reds, methods called by name
```

- **Natives** (C++ → script): `scripts/Cp2077Coop/CoopNative.reds` declares what `src/plugin/CoopSystem.*` and `ScriptTypes.hpp` register (`Host`, `Join`, `Leave`, `GetStatus`, `GetSetting`, `ActivateTimeField`, …, and for direct drive `SetPuppetAI`, `GetAnimStatus`, `StartAnimRecording`, `DumpAnimFunctions`, `MirrorAnimationsTo`, `SetMotionInputs`, `PlaceEntity`, `SetBodyOptions`/`GetBodyOptions`, `GetMotionInputs`, `OnBodyInitialize` for Codeware's callback, and for looks `ApplyMyLook`, `SetLookOptions`/`GetLookOptions`, `SetPuppetLooks`/`GetPuppetLooks`; structs `CoopLocalSample`, `CoopPuppetPose`). Keep both sides identical.
- **Bridge methods** (C++ calls script by name through `Red::CallVirtual`): `CaptureLocal`, `DrivePuppet`, `GetPuppetEntity`, `RemovePuppet`, `RemoveAllPuppets`, `OnPlayerJoined`, `ShowStatus`, `ApplyTimeRates` (in `src/plugin/RedGameAdapter.cpp`). A signature mismatch fails silently, so change both sides together.
- **Animation path:** `AnimCapture` (plugin) swaps entries in the engine's native-function handler table for the candidate functions it finds by name in RTTI (`Replicate*` on `gameObject`, native `SetAnimationParameter*`/`PushAnimationEvent`), reads the arguments, keeps inputs aimed at the local V (compared by address with the player system's player, so spawned `PlayerPuppet` bodies don't count), and calls the original. `RedGameAdapter::CaptureAnimInputs` drains them each frame → `SessionRunner` → `ClientSession::SendAnimInputs` (15 Hz, changes + events, full set every 2 s) → host relay → `ApplyRemoteAnimInputs` → `ApplyAnimInputs` on the puppet's animation controller. Motion values (`core/AnimMotion.hpp`) are applied in `DriveRemotePlayer` right after the bridge places the puppet.
- **Looks path (0.6, from CyberpunkMP):** once a second `RedGameAdapter::GetLocalAppearance` reads the local V's look (`Looks::CaptureLocal`: the items in its look slots, and its character customization state serialized with the game's own stream function) → `PlayerAppearance` (bodyGender, customizationState, equipment as `slot:item` hex, `core/LookItems.hpp`), sent when it changed → host stores and relays → `OnRemoteAppearance` keeps it per player (a changed look respawns the body) → `DriveRemotePlayer` sets pose flag 0x100 ("own look"; the bridge then picks the body by the remote's gender) and, 0.5 s after the body appears, `Looks::Apply`: third-person byte in `gamePuppetPS`, `TransactionSystem.GiveItem/AddItemToSlot` (RTTI, every optional parameter filled), customization parts through the world's entity appearance changer. Bots without a look get the local V's.
- **Portable vs game:** everything under `src/core`, `src/protocol`, `src/net`, `src/host`, `src/client` builds on Linux and is tested without the game; `IGameAdapter` (`src/client/GameAdapter.hpp`) is the only door to the game. `coop-sim`'s `SimPlayer` implements the same interface, which is how bots and tests exercise everything.

## 5. Repository map

| Path | What |
|---|---|
| `src/core` | Clock sync, time-field maths (`TimeFieldClock`), interpolation (players, vehicles), animation inputs (`AnimInput.hpp`) and motion values (`AnimMotion.hpp`), look items (`LookItems.hpp`), quantization, bit streams, SHA-256/HMAC/PBKDF2, `Version.hpp` |
| `src/protocol` | Message structs with one serializer for read and write (`Messages.hpp`), ids and `kProtocolVersion` (`Protocol.hpp`, currently 4) |
| `src/net` | `GnsTransport`: connections, lanes, impairment presets |
| `src/host` | `HostService`: handshake, roster, relays, vehicles, time fields |
| `src/client` | `ClientSession` (one player's session), `SessionRunner` (network thread), `GameAdapter.hpp` |
| `src/plugin` | Windows plugin: entry (`Main.cpp`), environment and dev instances (`Plugin.cpp`), `CoopSystem`, `RedGameAdapter`, `AnimCapture` (animation capture and apply), `Placement` (placing bodies), `BodySetup` (impostor / animation sets on bodies being built), `Looks` (a player's look on a body, from CyberpunkMP), `AnimDiagnostics` (graph dumps), `Settings` (coop.ini) |
| `src/tools/sim` | `coop-sim`: headless host and scripted bots |
| `scripts/Cp2077Coop` | redscript: `CoopNative.reds` (declarations), `CoopBridge.reds` (puppets: direct drive and AI-walking fallback, local capture, time fields) |
| `tweaks/Cp2077Coop` | TweakXL: `bodies.tweak` (the player bodies, male and female, as Character records: the default puppet and mirror body), `puppets.yaml` (the plain NPC record for `drive=ai`) |
| `LICENSE.md`, `NOTICE.md` | CyberpunkMP's license with this project's attribution; what was taken from CyberpunkMP and from where |
| `cet/coop-dev` | Dev panel: Session tab, Direct drive tab (mirror test, recording, function list, motion-input names), Probes tab (open questions only; older probes are in the git history up to 0.4.1) |
| `config/coop.ini` | Settings template, installed as `coop.ini.example` |
| `tests` | Unit and integration tests (real UDP on localhost and a deterministic in-memory network) |
| `tools/dev` | `bootstrap.ps1` (git, pinned SDKs, configure), `run-two.ps1` |
| `tools/xmake/install_game.lua` | Copies the built mod into the game folder file by file, into our own folders only; never deletes |
| `tools/reds-check` | Type-checks the scripts with redscript's compiler against hand-written stand-ins (`game-stubs.reds`) |
| `tools/plugin-check` | Compiles the Windows plugin on Linux against the real SDK headers (zig) |
| `.github/workflows/ci.yml` | Linux and Windows build and test on every push; Windows uploads the assembled mod |

## 6. Build, install, test

**Windows (the game PC):**
```powershell
powershell -ExecutionPolicy Bypass -File tools\dev\bootstrap.ps1   # once: git, pinned SDK submodules, configure
xmake f --game_dir="C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077"   # once
xmake                       # builds plugin, coop-sim, coop-tests; installs into the game; ends with "nothing deleted"
xmake run coop-tests
```
Never go back to copying whole folders into the game: xmake's `os.cp` deletes an existing destination folder first, which once wiped `bin\x64`, `r6\scripts` and more.

**Linux (portable parts):** `xmake -y && xmake run coop-tests`. Sanitizers: `xmake f -m releasedbg --policies=build.sanitizer.address,build.sanitizer.undefined --cxflags=-fno-sanitize=vptr -y && xmake -r && ASAN_OPTIONS=detect_leaks=0 xmake run coop-tests`, then `xmake f -c -m debug -y && xmake -r` to go back. `-fno-sanitize=vptr` is needed because GameNetworkingSockets is built without the type information UBSan's vptr check expects. Under the sanitizers' slowdown the clock-sync check in "session: host player and three clients see each other" can miss its 5 ms tolerance; that's timing, not a memory error. ThreadSanitizer (`build.sanitizer.thread`) reports races inside GameNetworkingSockets, which isn't built for it; ours are clean.

**Checks without the game:**
- Scripts: `tools/reds-check/check.sh`. Every game function the scripts call needs a stand-in in `game-stubs.reds`, with the signature taken from an RTTI dump of the real type (CET `DumpType`).
- Plugin: `tools/plugin-check/check.sh` (needs `pip install ziglang` and the SDK headers).
- Dev panel: parse `cet/coop-dev/init.lua` with any Lua 5.1 parser (`python3 -m pip install luaparser`).

**In the game:** [07-testing-guide.md](07-testing-guide.md); GitHub: [10-github-setup.md](10-github-setup.md). Fake players: `coop-sim client --connect 127.0.0.1:27077 --bots 2 --script follow [--sandevistan 0.25,6,15] [--impair typical]`; a fake host: `coop-sim host --bot circle`; vehicles: `--bot drive` / `--script ride`. Two games: start the game twice; the second becomes dev instance 2 (player "V 2") automatically.

## 7. Settings (`red4ext/plugins/Cp2077Coop/coop.ini`)

`[player] name`, `[network] port, impairment`, `[dev] allowSimClients, verbose`, `[time] applyToGame` (default true), `[puppet] drive` (`direct` default, or `ai`), `place` (`auto` default: transform, AI teleport, teleport; or `teleport`, `transform`, `aiteleport`), `switchOff` (component classes switched off with the AI, default `moveComponent`), `recordMale, recordFemale` (default `Cp2077Coop.Character.PlayerBody_Male/_Female` with looks on, `Character.TPP_Player_Cutscene_Male/Female` with `[look] apply=false`; `Character.TPP_Player` is the female player body; the plain NPC is `Cp2077Coop.Character.RemotePlayer`; never a bare template path), `addImpostor` (default false since round J; `useVGraph` was removed in 0.5.8, it crashed), `borrowAnimsets` (default false: lend NPC bodies the local V's gameplay animation sets), `[look] apply` (default true: puppets get their player's look and a body of their gender), `thirdPerson, items, customization` (default true: the look's three steps), `[anim] capture, apply` (default true), `applyVia` (`events` default, or `controller`), `movementFeature` (default true: the motion values also go out as the player graph's `playerLocomotion` feature), `tppFeature` (default false since round M: `TPPRepresentation { IsActive }` T-poses the player body), `speedInput, directionInput, verticalSpeedInput, turnRateInput, movingInput` (graph input names for motion values; defaults `speed_horizontal+desired_speed_horizontal`, `move_direction`, `speed_vertical`, `rotation_speed_yaw`, none; empty = default, `none` = not sent, `-name` = negated, `a+b` = both).

## 8. Spikes: where the research stands

Full results with evidence: [08-spike-results.md](08-spike-results.md).

| Spike | Result | Open |
|---|---|---|
| S1 body | V-lookalike = `TPP_Player_Cutscene_*`; it copies the *local* V through `gameImpostorComponent`; only the one matching the local V's body spawns; items can be put on it; its graph isn't V's. `TPP_Player` has V's graph but shows only a neck (round I) | Round P: the player body through a Character record with each player's look (CyberpunkMP's method: customization state, items, third-person flag) |
| S1 vehicles | Spawn, teleport (also every frame, smooth), NPC as driver or passenger (`AIMountCommand`) all work | Local V as a passenger by script (`MountingFacility.Mount`) |
| S2 targets | Plain NPC body: per-agent hostility is enough. Lookalike (no senses or visible object): needs threat injection | Puppets must not fight on their own (S2d) |
| S3 movement | AI move commands walk an NPC with animation; placing a body works with its `moveComponent` off (transform or single teleports; rounds G–H); captured inputs animate a body with V's graph (round I) | Motion inputs (walk speed and direction) on a placed body: round J |
| S8 time | Global dilation, V exemption, per-entity rate ignoring the global one: all confirmed and built in | Connect the game's own Sandevistan/Kerenzikov; smooth ramps |
| S13 two games | Starting the game twice works | Separate saves for instance 2 |
| S4–S7, S9–S12 | Not run | See [04 §3](04-feasibility-and-risks.md#3-reverse-engineering-spikes-do-these-first) |

## 9. Next steps, in order

1. **Round P results** ([07](07-testing-guide.md) T4m and T5; logs and dumps are read from the game folder through this chat's link):
   - If the record body spawns and the look works: puppets are done for looks; check two games (T6) so each sees the other's V.
   - If a look step crashes: the `look:` lines in `red4ext/logs` name the step. Third person: the `gamePuppetPS` byte (layout changed since 2.2?). Items: the parameter names of `AddItemToSlot` on 2.31. Customization: the virtual-table slots of `gameuiICharacterCustomizationState` or the appearance changer's index (34) / function. Keep that step off (`coop.ini [look]`) and find the 2.31 equivalent.
   - If a game function is `MISSING` in the status: its address hash changed since 2.2; find it in `bin/x64/cyberpunk2077_addresses.json` by its symbol.
   - If the head still turns first person: compare with CyberpunkMP's edited lookalike template (`code/assets/Archives/source/archive/mods/cyberpunkmp/player_ma_tpp_cutscene.ent`, an NPC body), which its `Character.MaMuppet` uses.
2. **Weapons in puppets' hands:** looks leave the weapon slots out; send the equipped weapon (planned message 0x030A) and put it in `WeaponRight` the same way.
3. **Passive puppets** (S2d) into the bridge.
4. **Vehicles in the game** (look at CyberpunkMP's `VehicleSystem` first: it already syncs cars and passengers between players): capture V's car (`GetMountedVehicle`, `IsPlayerDriver`, `GetRecordID`, `GetCurrentAppearanceName`, `GetLinearVelocity`) and register it; spawn proxies through `DynamicEntitySystem`; drive them by a teleport per frame; seat puppets with `AIMountCommand` (`seat_front_left` / `seat_front_right`); then the local V as a passenger. The portable side is done and tested. A placed puppet has to stop being placed while seated.
5. **Combat (M1):** mirror per-agent hostility towards remote players' puppets; hits both ways (S12); NPC ownership and handoff (S3, S4, S9).
6. **The game's own Sandevistan/Kerenzikov** → `ActivateTimeField`, and the game's other dilation sources ([01 §8.9](01-architecture.md#89-other-sources-of-time-dilation)).
7. Dev instance 2 with its own save folder.

## 10. Gotchas we hit (so you don't hit them again)

- **CET names:** `CName.new("")` is a real name, not the empty one; only `"None"` is 0. Passing `""` as an ease curve crashed the game. In redscript both `n""` and `n"None"` are 0.
- **redscript 1.0:** class fields need `public` to be set from outside; reading a field of a temporary (`Foo().Bar`) is an error (use a local); one bad call breaks every script, and the game then starts without script mods (the panel shows "SCRIPTS NOT LOADED"; the reason is in `r6\logs\redscript_rCURRENT.log`).
- **Spawned bodies:** `DynamicEntitySystem.GetEntity` is null until spawned; a freshly spawned body reports a wrong position for a moment (don't judge distance in the first seconds); a female lookalike won't spawn for a male V.
- **GameNetworkingSockets:** `RunCallbacks` dispatches the global status callback without its own lock; destroying a listen socket kills its child connections, so a quitting host keeps its transport for 500 ms to deliver "session closed".
- **xmake/tbox:** `os.cp` with a folder source deletes the destination folder first (see §6).
- **Time:** apply dilation only when it changes; a session that ends must hand the game normal time (the runner does that, with a test).
- **Bots:** `--script follow` bots trail V by 2.5–4 m and move at most 5 m/s by design, so they make puppets look later than real players.
- **Timing tests:** the tests use real UDP on localhost. With every CPU core saturated by something else, the clock-sync check (15 ms) and the frozen-main-thread test (1.5 s timeouts) can fail; rerun on an idle machine before suspecting the code.
- **Native hooks:** `AnimCapture` replaces handler-table entries (`CBaseFunction_Handlers[func->GetRegIndex()]`), never game code, and puts them back on unload. Only native functions can be hooked this way; script functions are listed as "script functions (not captured)" in the status. To read the arguments, the hook runs the frame's argument opcodes into its own storage and rewinds the frame (`code`, `data`, `dataType`, `currentParam`, `useDirectData`) before calling the original, so arguments are evaluated twice.
- **Placing NPCs:** `TeleportationFacility.Teleport` doesn't move a spawned NPC whose AI is on (S3). Direct drive switches the AI off first; whether that is enough is round F. Any placement every frame has to skip no-op calls (the bridge only places when the pose moved, turned or drifted).
- **CET and plugin functions:** check that a native function exists before calling it from CET (`has(system, "Name")` in the panel), so an older plugin build shows a message instead of erroring every frame.
- **Script stand-ins must follow the game's class hierarchy:** `tools/reds-check` only knows what `game-stubs.reds` declares. In 0.5.0 the stand-ins had `TimeDilatable` above `GameObject` (it is below: `GameObject` ← `TimeDilatable` ← `PuppetBase` ← `gamePuppet`), so a `SetIndividualTimeDilation` call on a `GameObject` passed the check and broke compilation in the game. Before calling a game function on a new receiver type, check where the function is declared (RED4ext.SDK's `Natives/Generated` headers give the parent of every native class; a CET `DumpType` lists inherited functions too, so compare with a sibling class).
- **NPCs don't take teleports:** `TeleportationFacility.Teleport` moves V and cars but not a spawned NPC, whether its AI is on or off (rounds A and F). Use the plugin's `PlaceEntity` methods (Codeware `SetWorldTransform`, `AITeleportCommand`); the dev panel's placement test says which work.
- **RED4ext.SDK's handler table:** `CBaseFunction_Handlers` read as a pointer to the table (as the SDK's own `GetHandler` does) gives null on 2.31. `AnimCapture` checks both readings against `gameObject`'s native functions and reports the scores in its status.
- **RedLib calls:** `Red::CallVirtual`/`CallStatic` need every parameter, optional ones included (pass `nullptr`), plus the return value first when the function has one, or they silently return false.
- **Where V's animation inputs come from:** not the animation controller's natives (0 calls in round G). The game's `AnimationControllerComponent.ApplyFeature/SetInput*/PushEvent` helpers are script functions that queue `AnimInputSetter*` / `AnimExternalEvent` events on the entity, and the player's state machines call `gamestateMachineGameScriptInterface.SetAnimationParameter*` / `PushAnimationEvent`. `AnimCapture` hooks those (plus `Entity.QueueEvent`, filtered to the local V before any argument is read).
- **Teleports every frame:** a single teleport moves a third-person V (round G), one per frame doesn't (round F); placement every frame uses Codeware's `SetWorldTransform`.
- **Placing an NPC needs its movement component off:** teleport and transform move a spawned NPC only with `moveComponent` switched off (round H); `[puppet] switchOff=moveComponent` is the default and is applied even to bodies without an AI.
- **Locomotion speed is not scripted:** V's walk/run speed reaches V's graph natively; only states (crouch, jump, aim, weapon, events) pass through scripts. A placed body gets speed and direction as motion inputs.
- **Player bodies spawned by script:** `Character.TPP_Player` (and the photo-mode and replacer records) use V's own animation graph, but show only a neck when spawned through `DynamicEntitySystem`; the cutscene lookalike looks like V through its impostor component but has another graph (round I). Since spawned bodies are `PlayerPuppet`s too, anything that recognises the local V by class picks them up: `AnimCapture` compares with the player system's player by address.
- **Changing a body while it's built:** Codeware's `CallbackSystem` `Entity/Initialize` event (target `DynamicEntityTarget.Tag(...)`) fires while components are requested, the only point where components can still be added or a resource reference (the root graph) changed. `BodySetup` registers `CoopSystem.OnBodyInitialize` there for the tags `Cp2077Coop.Puppet` and `CoopMirror`; anything spawned without those tags is never touched.
- **Empty values in coop.ini count:** `Settings::Get` returns an empty value as set, not as missing. For motion inputs an empty value now means the default (old templates had empty lines), and `none` turns one off.
- **`Red::AsHandle<T, U>(this)` with U ≠ T recurses forever:** it returns a `WeakHandle<T>&` converted to `Handle<U>` through `WeakHandle::operator Handle<U>()`, whose body (`return *this;`) picks itself again (found in review before 0.5.5 shipped; GCC and Clang both recurse). Lock the weak handle as its own type and let the reference conversion do the rest: `const Red::Handle<CoopSystem> self = Red::AsWeakHandle(this).Lock();`, then pass `self` where a `Handle<IScriptable>` is expected.
- **A Codeware callback without targets runs for every entity:** add the targets right after `RegisterCallback`, ignore calls until they're on, and `Unregister` the handler if adding them fails (`BodySetup` does all three, and never touches the local V).
- **Engine objects in statics:** a resource reference (or any handle) in a function-local static is released during the DLL's static destruction, after the engine is gone. `BodySetup`'s singleton is never destroyed for that reason, and it drops the V graph when the world detaches.
- **A player body is dressed by items:** V's head, body, hair and clothes are meshes brought by items in attachment slots (round I dump) [inference until round K]; a spawned `TPP_Player` without them is only a neck, and an impostor component doesn't change that (round J).
- **A placed player body doesn't walk from named floats:** V's root graph gets its walking from the feature `playerLocomotion` (`animAnimFeature_PlayerMovement`), set natively by the player's movement; `speed_horizontal` and the other floats alone leave the legs still (round K). [VERIFY] round L that the built feature makes it run.
- **A spawned player body thinks it is first person:** it swaps `Items.PlayerMaTppHead` in `TppHead` for `Items.PlayerFppHead` within a second, and its headgear doesn't show (round K).
- **The walking of V's graph is the `playerLocomotion` feature:** built from the motion values (world-space directions) it makes a placed player body run (round L); the named floats alone don't.
- **Never give an NPC body V's animation graph:** swapping the lookalike's root graph for V's crashed the game every time (round L); the option is gone.
- **A spawned player body is first-person and female by default** (`pwa` meshes, first-person head in `TppHead`), round L.
- **Spawned player bodies ignore the TPP representation events** and T-pose with `TPPRepresentation { IsActive = true }` (round M).
- **Resource paths:** Codeware ships `red4ext/plugins/Codeware/Data/KnownHashes.txt` (resource paths it knows, ~130k lines: entity templates, meshes; no animation sets); handy to turn a path hash from a dump into a name.
- **The lookalike's graph doesn't walk:** it has third-person V animation sets ("Player TPP Animation Setup") but no walking logic; lending it V's first-person sets changes nothing (round N).
- **Spawning by template path:** Codeware's `DynamicEntitySpec.templatePath` can't be built from a string in CET; `CoopSystem.SetSpawnTemplate(spec, path)` writes the path hash (`RED4ext::ResourcePath`, the engine's sanitized FNV-1a 64).
- **Version in three places:** `src/core/Version.hpp` (`kVersionMajor/Minor/Patch`, `kVersionString`), `xmake.lua` (`set_version`) and `cet/coop-dev/init.lua` (`PANEL_VERSION`). The plugin logs its version when it loads and the dev panel compares it with its own (round O started with a stale build installed: the panel showed none of the new bodies).
- **Never spawn a character from a bare template path:** Codeware turns `DynamicEntitySpec.templatePath` into a `SpawnableObject` record, and a `PlayerPuppet` built without a Character record crashed the game (round O, `player_ma_tpp.ent` and `player_ma_tpp_reflexion.ent`). Give it a TweakXL Character record with `entityTemplatePath` and one `genders` entry (`tweaks/Cp2077Coop/bodies.tweak`, CyberpunkMP's pattern). `SetSpawnTemplate` is only for things that aren't characters.
- **RED4ext.SDK's `UniversalRelocBase::Resolve` ends the game** (a message box, then `TerminateProcess`) when an address hash isn't in the game's address list. Code that takes hashes from an older game version (CyberpunkMP's are for 2.2) calls RED4ext's `RED4ext_ResolveAddress` export directly and treats 0 as "not found" (`Looks.cpp`).
- **Pointers read at offsets from older code:** check the class with `IsA` before using them, inside SEH on MSVC (`SafeIsA` in `Looks.cpp`), so a moved field reads as "not found" instead of crashing.
- **Script natives with optional parameters from C++:** `Red::CallVirtual` needs every parameter; `CallFilled` in `Looks.cpp` fills the ones after the given leading arguments with defaults (or named values, e.g. `garmentAppearanceName`), checking the leading types by name.
- **Player bodies have no AI controller:** `SetPuppetAI` returns false for them but still switches their movement component, which is what placement needs; the bridge counts that as switched (0.6).
- **Building on Linux in a container as root:** xmake refuses unless `XMAKE_ROOT=y` is set.
