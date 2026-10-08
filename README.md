# Cp2077Coop

Story co-op for Cyberpunk 2077 (base game + Phantom Liberty) for 2–4 players, in the spirit of Elden Ring's Seamless Co-op. `Cp2077Coop` is a working name.

> Not affiliated with or endorsed by CD PROJEKT RED. Every player must own the game. No game files or CDPR assets are included. Built from scratch on MIT/BSD/Apache-licensed libraries; see [docs/01-architecture.md §1](docs/01-architecture.md#1-foundation-a-clean-build).

**Status: M0b in progress, M1 started.** The networking runs on its own thread inside the game and players connect. **New in 0.5 ("direct drive"):** remote players are shown as the game's third-person V, with its AI off, placed every frame at the other player's position and facing, and animated with the animation inputs the plugin captures from the other player's V; a puppet that doesn't follow the placements falls back to AI walking by itself. In the game so far (rounds F–I): placing a body every frame works, the capture sees your V's inputs, and the player body dressed in your V's items runs and crouches with you, but stays a first-person woman's body; the cutscene lookalike has your V's whole look but a graph that can't walk; the next round tries the male player body with the lookalike's impostor ([steps](docs/07-testing-guide.md)). Sandevistan/Kerenzikov slow motion is applied in the game, and two copies of the game can play together on one PC. Vehicle sync works between the headless tools ([results so far](docs/08-spike-results.md), [roadmap](docs/06-roadmap.md)).

## Design documents

| Doc | Contents |
|---|---|
| [docs/README.md](docs/README.md) | Index, decisions, open questions |
| [docs/01-architecture.md](docs/01-architecture.md) | Architecture, authority model, time fields, replication, threading, failure modes |
| [docs/02-systems.md](docs/02-systems.md) | System-by-system design |
| [docs/03-network-protocol.md](docs/03-network-protocol.md) | Message catalog |
| [docs/04-feasibility-and-risks.md](docs/04-feasibility-and-risks.md) | Ratings, reverse-engineering spikes |
| [docs/05-local-testing.md](docs/05-local-testing.md) | Testing with one PC and one copy |
| [docs/06-roadmap.md](docs/06-roadmap.md) | Milestones M0–M5 |
| [docs/07-testing-guide.md](docs/07-testing-guide.md) | **Step by step: testing direct-drive puppets in the game** |
| [docs/08-spike-results.md](docs/08-spike-results.md) | What the in-game experiments showed and decided |
| [docs/09-development-handoff.md](docs/09-development-handoff.md) | **Start here to continue development:** rules, status, how it fits together, build and checks, next steps, gotchas |
| [docs/10-github-setup.md](docs/10-github-setup.md) | Putting the project on GitHub, and committing each update |

## Repository layout

```
src/core        clock sync, time fields, interpolation, animation inputs and motion values, bit streams, crypto (portable)
src/protocol    messages and codec (portable)
src/net         GameNetworkingSockets transport with lanes and impairment (portable)
src/host        HostService: handshake, roster, relay (portable)
src/client      ClientSession: one player's side of a session (portable)
src/tools/sim   coop-sim: headless host and scripted fake players
src/plugin      RED4ext plugin (Windows): CoopSystem game system, script bridge adapter, animation capture
scripts/        redscript bridge  -> r6/scripts/Cp2077Coop
tweaks/         TweakXL records   -> r6/tweaks/Cp2077Coop
cet/coop-dev    CET dev panel: host/join, mirror test, animation tools, probes (development only)
config/         coop.ini template
tests/          unit tests and loopback integration tests
tools/dev       bootstrap and two-instance launcher
tools/reds-check, tools/plugin-check   script and plugin checks without the game
```

## Building (Windows)

Requirements: Visual Studio 2022 ("Desktop development with C++"), [xmake](https://xmake.io), git.

```powershell
powershell -ExecutionPolicy Bypass -File tools\dev\bootstrap.ps1   # git init, pinned SDK submodules, configure
xmake                                                               # builds plugin, coop-sim, coop-tests
xmake run coop-tests                                                # unit + loopback integration tests
```

The first build downloads and compiles GameNetworkingSockets and its dependencies through xmake (several minutes).

To copy the mod straight into the game after each build:

```powershell
xmake f --game_dir="C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077"
xmake
```

The copy goes file by file into the mod's own four folders (`red4ext\plugins\Cp2077Coop`, `r6\scripts\Cp2077Coop`, `r6\tweaks\Cp2077Coop`, `bin\x64\plugins\cyber_engine_tweaks\mods\coop-dev`), never deletes anything, and refuses to write anywhere else (`tools/xmake/install_game.lua`). Builds from before this change copied whole folders, which made xmake delete the game's existing folders first; if you used one of those, see "Recovering from the old copy step" below.

Otherwise the files are assembled in `build\package\` in the game's folder layout.

The portable targets (everything except the plugin) also build on Linux with `xmake && xmake run coop-tests`.

### Game requirements (patch 2.31 only)

RED4ext, redscript, Codeware, TweakXL, and Cyber Engine Tweaks for the dev panel. The plugin is pinned to game build 2.31; RED4ext won't load it on any other build.

## Trying it

### One game + fake players (works on any setup)

1. Start the game, load a save, open the CET overlay → **Co-op (dev)** → **Host**.
2. In a terminal: `build\windows\x64\releasedbg\coop-sim.exe client --connect 127.0.0.1:27077 --bots 2 --script follow`
3. Two third-person Vs appear where the bots are and move with them (the bots follow you). They look like your own V for now, and glide, because bots send no animation inputs. Status is in the panel; details in `red4ext\logs`.

If the panel says **SCRIPTS NOT LOADED**, redscript failed to compile the mod's scripts (the game then starts without any script mods). The reason is in `r6\logs\redscript_rCURRENT.log`; the usual causes are a missing or outdated Codeware, or a missing `r6\scripts\Cp2077Coop` folder. Until the scripts load, your position isn't sent, so the bots show "no data yet".

Other things to try:

- `--impair typical` (or `good`, `bad`, `awful`) on the coop-sim command to add lag, jitter and loss.
- Joining a headless host instead of hosting: run `coop-sim host --bot circle`, then press **Join** in the panel. A fake host walks in circles around you.
- `coop-sim host --password secret`, then join with a wrong password to see the rejection.
- Sandevistan between headless tools: `coop-sim host --bot circle --center 0,0,0` and `coop-sim client --connect 127.0.0.1:27077 --bots 2 --script line --center 10,0,0 --sandevistan 0.25,8,20`. Every 20 s the first client bot activates for 8 s: it reports `world x0.25, me x1.00`, the bots near it `world x0.25, me x0.25`. Add a third client with `--center 4000,0,0` to see that far players stay at normal speed.
- In the game: the panel's **Sandevistan x0.25 for 8 s** button activates one for your V; bots near you slow down (watch their coop-sim output), and a bot's Sandevistan shows up in the panel's "Time:" line. The mod slows your world with it and keeps the activating player's puppet at full speed (`coop.ini` `[time] applyToGame`).
- Vehicles between headless tools: `coop-sim host --bot drive --center 0,0,0 --radius 25` and, in a second terminal, `coop-sim client --connect 127.0.0.1:27077 --script ride`. The host's bot summons a car and drives in circles; the client's bot takes a passenger seat. Both print the car's owner, epoch, seats and position. (In the game, cars from bots don't appear yet.)

### Two instances on one PC

Start the game twice (S13: Steam allows it); the second one becomes player "V 2" automatically. Host in one, join `127.0.0.1:27077` from the other. Steps and the save-folder caveat: [docs/07-testing-guide.md](docs/07-testing-guide.md) T5; background: [docs/05-local-testing.md](docs/05-local-testing.md).

## Recovering from the old copy step

Builds before 2026-10-07 23:00 copied the mod into the game folder by folder, and xmake deletes an existing destination folder before copying one. That wiped `bin\x64` (game files plus the RED4ext and CET loaders), `bin\x64\plugins` (CET and its mods), `r6\scripts`, `r6\tweaks` and `red4ext\plugins`. Save games live outside the game folder and were not touched.

1. Update to the current files before building again.
2. Steam → Cyberpunk 2077 → Properties → Installed Files → Verify integrity of game files (restores the game's own files).
3. Reinstall the frameworks and any other mods that had files in those folders: RED4ext, Cyber Engine Tweaks, redscript, Codeware, TweakXL, ArchiveXL, CET mods, script mods.
4. Build again; the build should end with "copied N mod file(s) ... (nothing deleted)".

## Next test round (please send back the results)

Step by step in [docs/07-testing-guide.md](docs/07-testing-guide.md). In short:

| Test | What to check |
|---|---|
| Male player body + impostor (T4l) | Does it look like your V and walk with you? |

Results so far (rounds A–N; round E was skipped): [docs/08-spike-results.md](docs/08-spike-results.md).

## What is and isn't verified

- **In the game (your tests, 2026-10-07 and 08):** the plugin builds with Visual Studio and loads on 2.31; hosting works, fake players join from another process, clock sync works, the script bridge loads and spawns puppets. A spawned NPC walks with AI move commands; a V-lookalike record exists; spawned cars can be moved by teleport and NPCs seated in them; the game's slow motion follows a bot's Sandevistan with V exempt during V's own ([details](docs/08-spike-results.md)). Direct drive (rounds F–I): a placed body follows every frame with its movement component off; the capture sees your V's inputs; the player body animates with them. The player body dressed in your V's items runs and crouches with you (rounds K–L). Not yet seen: a body that both looks fully like your V and animates, and puppets between two games.
- **Portable code:** 60 tests pass on Linux (repeated runs, also with every CPU core busy). They cover real UDP sessions, animation inputs (codec, relay with events exactly once, late joiners, motion values), the network thread (the game adapter is only called on the main thread; a 3 s main-thread freeze keeps the session alive), vehicle sync and time fields on a deterministic in-memory network (seats, handoff, stale epochs, forged spawns, players leaving, late joiners; slow-motion rates identical on every machine, real slowed movement, proportional overlaps, easing in and out of range, world clocks agreeing exactly). AddressSanitizer and UndefinedBehaviorSanitizer find no errors (one clock-sync timing check can miss its tolerance under their slowdown). ThreadSanitizer finds no races in our code; its remaining reports are inside GameNetworkingSockets, which isn't built for it. How to run them: [docs/09](docs/09-development-handoff.md) §6.
- **Across processes:** a host with three simulated players ran with the "typical" network preset (about 120 ms ping); a driving host plus a riding client shared a car with matching positions on both sides; a client bot's Sandevistan slowed the host's bot and its neighbour to x0.25 while a far player stayed at normal speed.
- **CI:** `.github/workflows/ci.yml` builds and tests on Linux and Windows on every push and uploads the built mod; it runs once the repository is on GitHub (not run yet).
- **Plugin:** compiles against the real RED4ext.SDK and RedLib headers (`tools/plugin-check/check.sh` on Linux, MSVC on your PC). The animation capture builds with MSVC and runs in the game (round H); the body setup of 0.5.5 runs (Codeware callback registered, round J).
- The redscript bridge compiles and loads in the game (patch 2.31). Script changes are type-checked with redscript's own compiler before they ship (`tools/reds-check/check.sh`, needs git and Rust), against hand-written stand-ins for the game functions we call, so it catches mistakes in our code but can't prove a game function exists.

## License

MIT, see [LICENSE](LICENSE).
