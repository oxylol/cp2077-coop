# 09 — Development handoff: what you need to keep going

State at version **0.4.1** (2026-10-08). Read this first; the other documents go deeper.

## 1. What this is

A Seamless-Co-op-style mod for Cyberpunk 2077 (base game + Phantom Liberty, **patch 2.31 only**) for 2–4 players: the host's save owns the world, every client keeps their own character, no tether or teleports, one logical world. Design: [01](01-architecture.md) (architecture), [02](02-systems.md) (every game system), [03](03-network-protocol.md) (messages), [04](04-feasibility-and-risks.md) (ratings and spikes), [06](06-roadmap.md) (milestones M0–M5).

## 2. Ground rules (not negotiable)

- **Legal:** every player owns the game. No CD PROJEKT RED assets or game files in the repository or the package; follow CDPR's fan content and modding guidelines. Never bypass DRM or account checks. The `probe-*.txt` dumps from the game are RTTI dumps: keep them out of the repository.
- **Clean room:** no code from other Cyberpunk 2077 multiplayer projects is used, studied or linked. CyberpunkMP's license forbids studying it for a competing product, so its code is never read ([01 §1](01-architecture.md#1-foundation-a-clean-build)). Engine behaviour is learned only from our own in-game experiments and public modding documentation.
- **Saves are never at risk:** back up `%USERPROFILE%\Saved Games\CD Projekt Red\Cyberpunk 2077` before co-op testing; never save during experiments; with two games running, never save manually (they share the save folder).
- **Platform:** RED4ext API v1 on 2.31 (supported; no need to re-check). Requirements for players: RED4ext, redscript, Codeware, TweakXL; CET only for the dev panel.
- **How we work:** compromises are written down where they're made; engine assumptions are marked [VERIFY] until a spike confirms them. New engine calls are tried in the CET dev panel first (wrapped in `pcall`, with a log line written *before* each risky call, so a crash names its cause), and only confirmed calls go into the redscript bridge: one wrong call there stops every script from compiling.
- **User decisions so far:** Sandevistan/Kerenzikov slow everyone nearby (activator at full speed, overlaps proportional, local scope by default); each player sees their own Johnny; loot instanced per player by default; MIT license; ask before large code changes; prefer hooking the game's systems over reimplementing them. Test setup: one Steam copy, one PC with 16 GB RAM and 12 GB VRAM.

## 3. Status

| Area | State |
|---|---|
| Networking (portable C++) | Done for M0/M1 scope: GameNetworkingSockets transport with lanes and impairment, handshake with password and version checks, clock sync, 30 Hz player state with interpolation, network thread (`SessionRunner`) with the game touched only on the main thread |
| Host | Roster, relay, seat arbitration and ownership handoff for vehicles, time-field validation and proximity groups (150 m), late-joiner snapshots |
| In the game | Plugin loads on 2.31; host/join from the dev panel; fake players (`coop-sim`) and a second game join; remote players appear as **plain NPCs that walk, run and sprint with AI move commands** (V-lookalikes slide, so they are opt-in); **slow motion applied in the game** (global dilation, V exempt while activating, puppets at their players' rates, normal time when a session ends) |
| Headless only so far | Vehicle sync (spawn, seats, handoff, late joiners) between `coop-sim` processes; time fields between bots |
| Not started | Combat replication, NPC ownership, quests/scenes/facts (M2–M3), saves and join modes, UI |
| Tests | 48 (`xmake run coop-tests`); no AddressSanitizer/UBSan errors; ThreadSanitizer clean for our code (§6) |

Compromises in place: puppets don't look like their players (plain NPC body); puppets don't jump, vault or climb and don't turn while standing; puppets lag behind (AI walking; see S3d); a plain-NPC puppet joins fights on its own (S2d); slow-motion eases are applied as steps; the game's own Sandevistan isn't connected to the session yet; two games share saves.

## 4. How the pieces fit

```
game main thread                                   network thread (SessionRunner, 4 ms)
CoopSystem (RED4ext game system, ticks each frame)  ClientSession  <->  HostService (host only, in-process)
  -> SessionRunner::Pump()  ---- queued events -->        |
       -> RedGameAdapter (IGameAdapter)              GnsTransport (GameNetworkingSockets)
            -> CoopBridge.reds, methods called by name
```

- **Natives** (C++ → script): `scripts/Cp2077Coop/CoopNative.reds` declares what `src/plugin/CoopSystem.*` and `ScriptTypes.hpp` register (`Host`, `Join`, `Leave`, `GetStatus`, `GetSetting`, `ActivateTimeField`, …, structs `CoopLocalSample`, `CoopPuppetPose`). Keep both sides identical.
- **Bridge methods** (C++ calls script by name through `Red::CallVirtual`): `CaptureLocal`, `DrivePuppet`, `RemovePuppet`, `RemoveAllPuppets`, `OnPlayerJoined`, `ShowStatus`, `ApplyTimeRates` (in `src/plugin/RedGameAdapter.cpp`). A signature mismatch fails silently, so change both sides together.
- **Portable vs game:** everything under `src/core`, `src/protocol`, `src/net`, `src/host`, `src/client` builds on Linux and is tested without the game; `IGameAdapter` (`src/client/GameAdapter.hpp`) is the only door to the game. `coop-sim`'s `SimPlayer` implements the same interface, which is how bots and tests exercise everything.

## 5. Repository map

| Path | What |
|---|---|
| `src/core` | Clock sync, time-field maths (`TimeFieldClock`), interpolation (players, vehicles), quantization, bit streams, SHA-256/HMAC/PBKDF2, `Version.hpp` |
| `src/protocol` | Message structs with one serializer for read and write (`Messages.hpp`), ids and `kProtocolVersion` (`Protocol.hpp`, currently 3) |
| `src/net` | `GnsTransport`: connections, lanes, impairment presets |
| `src/host` | `HostService`: handshake, roster, relays, vehicles, time fields |
| `src/client` | `ClientSession` (one player's session), `SessionRunner` (network thread), `GameAdapter.hpp` |
| `src/plugin` | Windows plugin: entry (`Main.cpp`), environment and dev instances (`Plugin.cpp`), `CoopSystem`, `RedGameAdapter`, `Settings` (coop.ini) |
| `src/tools/sim` | `coop-sim`: headless host and scripted bots |
| `scripts/Cp2077Coop` | redscript: `CoopNative.reds` (declarations), `CoopBridge.reds` (puppets, local capture, time fields) |
| `tweaks/Cp2077Coop` | TweakXL: the plain NPC puppet record |
| `cet/coop-dev` | Dev panel: session controls, status, all spike probes |
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

**In the game:** [07-testing-guide.md](07-testing-guide.md) part B. Fake players: `coop-sim client --connect 127.0.0.1:27077 --bots 2 --script follow [--sandevistan 0.25,6,15] [--impair typical]`; a fake host: `coop-sim host --bot circle`; vehicles: `--bot drive` / `--script ride`. Two games: start the game twice; the second becomes dev instance 2 (player "V 2") automatically.

## 7. Settings (`red4ext/plugins/Cp2077Coop/coop.ini`)

`[player] name`, `[network] port, impairment`, `[dev] allowSimClients, verbose`, `[time] applyToGame` (default true), `[puppet] recordMale, recordFemale` (default: the plain NPC body; the V-lookalikes are `Character.TPP_Player_Cutscene_Male/Female`).

## 8. Spikes: where the research stands

Full results with evidence: [08-spike-results.md](08-spike-results.md).

| Spike | Result | Open |
|---|---|---|
| S1 body | V-lookalike = `TPP_Player_Cutscene_*`; it copies the *local* V through `gameImpostorComponent`; only the one matching the local V's body spawns; items can be put on it | Each puppet with its own player's looks (S1c `slotIDsToOmit`, or the bare `No_Impostor` body + items; faces are the hard part) |
| S1 vehicles | Spawn, teleport (also every frame, smooth), NPC as driver or passenger (`AIMountCommand`) all work | Local V as a passenger by script (`MountingFacility.Mount`) |
| S2 targets | Plain NPC body: per-agent hostility is enough. Lookalike (no senses or visible object): needs threat injection | Puppets must not fight on their own (S2d) |
| S3 movement | AI move commands walk an NPC with animation; teleports don't move spawned NPCs with AI on; lookalikes slide | Lower delay: AI teleport, direct drive with the AI off, follow with matched speed (S3d); animation inputs for a player body (S3c) |
| S8 time | Global dilation, V exemption, per-entity rate ignoring the global one: all confirmed and built in | Connect the game's own Sandevistan/Kerenzikov; smooth ramps |
| S13 two games | Starting the game twice works | Separate saves for instance 2 |
| S4–S7, S9–S12 | Not run | See [04 §3](04-feasibility-and-risks.md#3-reverse-engineering-spikes-do-these-first) |

## 9. Next steps, in order

1. **Round E results** (tests B3–B5 in [07](07-testing-guide.md)) decide the puppet movement: if a puppet can be moved directly with its AI off (S3d), build "proxy mode" (AI off, position from the interpolation buffer every frame, animation inputs from the network); otherwise follow with matched speed plus AI teleports for corrections.
2. **Passive puppets** (S2d) into the bridge.
3. **Vehicles in the game:** capture V's car (`GetMountedVehicle`, `IsPlayerDriver`, `GetRecordID`, `GetCurrentAppearanceName`, `GetLinearVelocity`) and register it; spawn proxies through `DynamicEntitySystem`; drive them by a teleport per frame; seat puppets with `AIMountCommand` (`seat_front_left` / `seat_front_right`); then the local V as a passenger. The portable side is done and tested.
4. **Combat (M1):** mirror per-agent hostility towards remote players' puppets; hits both ways (S12); NPC ownership and handoff (S3, S4, S9).
5. **Appearance:** S1c route or bare body + items; send customization and equipment in the appearance message (the fields exist).
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
