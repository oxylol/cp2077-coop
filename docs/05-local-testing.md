# 05 — Local testing with one PC and one game copy

Deliverable 5, part 1: the development test setup. The full testing plan (quest regression checklist, soak tests) follows with the roadmap.

Setup this is written for: **two PCs, one Steam copy of the game, testing on one PC with 16 GB RAM and 12 GB VRAM.**

**What that means in practice:** VRAM is fine for two instances at low settings, but 16 GB of RAM is not enough for two copies of Cyberpunk to run smoothly; the second instance will page to disk. So the everyday loop is **one game + the headless tools** (§3), and two instances are reserved for tests that truly need two engines (NPC handoff between two real simulations, scenes seen by a real second client). For those runs: lowest preset, 1280×720, background instance capped at 20–30 FPS, a 32 GB+ page file on an SSD, browsers and launchers closed. Upgrading to 32 GB RAM would make two-instance testing practical.

## 1. What one copy allows

| Setup | Possible with one copy | Notes |
|---|---|---|
| Two game instances on one PC, both yours | Yes, if the store lets the second process start (§2) | Primary setup |
| One game instance + headless simulated players | Yes, always | No second game needed (§3) |
| Second PC running headless tools, log viewer, network impairment | Yes | The second PC doesn't need the game |
| Game running on both PCs at the same time | Depends on your store's terms | Not planned around |
| Steam relay and invites between two Steam accounts | No | Needs two accounts that each own the game; spike S11 waits until a second tester joins |

Ground rule: we never bypass store DRM or account checks. If a store refuses a second concurrent instance, we test with the headless tools instead.

## 2. Two instances on one PC ("dev instance mode")

Dev builds of the plugin accept `-coopInstance=N` on the game's command line. Release builds don't contain any of this.

What the plugin does in instance N:

| Problem with a second instance | Dev instance mode | [VERIFY] (spike S13) |
|---|---|---|
| The game may refuse to start twice (single-instance guard) | Rename the guard object per instance, or, if it's checked before plugins load, a dev launcher closes instance 1's guard handle before starting instance 2 (standard multi-boxing technique; not a DRM mechanism) | What the guard is (named mutex, event or window lookup) and whether RED4ext loads early enough to hook it |
| Both instances share saves and settings | Redirect the game's Saved Games and LocalAppData lookups to `coop-dev/instance-N/` | Which API the game uses to locate those folders |
| Both write the same log files | Our logs get an instance suffix. Run CET only in instance 1 | — |
| The unfocused window may pause or throttle | Keep simulating and rendering when unfocused | Whether the game pauses or throttles in the background |
| Double audio | Mute every instance except 1 | — |
| Only the focused window gets input | Keyboard and mouse for instance 1, a gamepad for instance 2 if the game reads it unfocused; otherwise alternate focus or use scripted input | Whether gamepad input reaches an unfocused instance |
| Setup tedium | Instance 2 auto-loads a configured save and auto-connects to `127.0.0.1` | — |

`tools/dev/run-two.ps1` starts instance 1 as host, waits for the main menu, starts instance 2 as client, places the windows side by side and titles them `[Co-op 1]` and `[Co-op 2]`.

### Store notes

- **Steam (your copy):** depends on whether Steam lets one account run the same game twice at once. If it refuses, we use §3. Test: start the game from Steam, then run `Cyberpunk2077.exe` directly from `bin\x64` and note exactly what happens (nothing, an error dialog, a Steam message, or a second window).
- **GOG:** DRM-free; expected to work.
- **Epic:** the launcher-issued sign-in may block a direct second launch.

### Hardware for two instances (estimate)

- **RAM:** 32 GB recommended; 16 GB will likely page heavily.
- **GPU:** 10–12 GB VRAM or more.
- **Settings for both instances:** Low preset, 1280×720 windowed, ray tracing off, upscaler on performance, background instance capped at 30 FPS.

## 3. Headless simulated players (no second game)

Two console tools in `src/tools/sim`, built in M0. They speak the real protocol, so the game can't tell them from real players.

- **SimClient** joins your game as a fake player. It appears as a puppet; walks scripted paths, follows you, sprints, jumps, enters and leaves vehicle seats, fires and sends hits, activates a Sandevistan, joins and leaves time fields, disconnects and rejoins. Scenarios are small script files, so a test is repeatable.
- **SimHost** plays host to your game running as a client. It sends a world baseline, facts, journal updates, entity spawns and snapshots (from recordings), scene starts and time-field activations, and checks your client's responses.
- **Record and replay:** every real session writes a netlog; SimHost and SimClient can replay it to reproduce a bug.

Both can run on the same PC as the game or on the second PC (real network path).

### What each setup can test

| Area | 1 game (host) + SimClients | 1 game (client) + SimHost | 2 games on one PC |
|---|---|---|---|
| Handshake, version and manifest checks, password | ✔ | ✔ | ✔ |
| Puppet spawn, appearance, locomotion, vehicle seats (seeing others) | ✔ | — | ✔ |
| Host arbitration, cell ownership, relay with 3 fake clients | ✔ | — | ✔ (1 client) |
| Ownership handoff of real NPCs | Partly (sim players can't simulate NPCs) | Partly (from recordings) | ✔ |
| Combat both ways, aggro on puppets | Partly (puppets as targets; scripted hits) | — | ✔ |
| Time fields (Sandevistan) | ✔ (sim activations, your world slows) | ✔ (follower controller) | ✔ |
| Client follower mode, fact/journal apply, scene observer replay | — | ✔ (from recordings) | ✔ |
| Quest leasing | — | Partly | ✔ |
| Four-player load and bandwidth | ✔ (3 SimClients) | — | — |

## 4. Network conditions on one PC

- **Built-in impairment:** GameNetworkingSockets has fake lag, jitter, loss, reordering, duplication and rate limiting, and it works on loopback. These settings are **global to a process**, not per connection. The dev panel and `coop-sim --impair` expose presets:

| Preset | RTT | Jitter | Loss | Upload cap |
|---|---|---|---|---|
| LAN | 2 ms | 0 | 0 % | — |
| Good home | 40 ms | 5 ms | 0.5 % | — |
| Typical | 120 ms | 15 ms | 1 % | 5 Mbit/s |
| Bad | 200 ms | 30 ms | 3 % | 2 Mbit/s |
| Awful | 300 ms | 60 ms | 5 % (bursty) | 1 Mbit/s |

- Because the settings are per process, run each SimClient as its own process to give players different conditions (one "far away", one "near"). The lag applies to both send and receive, so a preset's RTT is the round trip one impaired process sees.
- **External check:** clumsy (WinDivert) on the real path between the two PCs, to confirm the built-in numbers behave the same as a real network.

## 5. Logging and desync checks

Every instance and sim tool writes a netlog and once-per-second state hashes (01 §13). The desync viewer merges the logs of all participants onto one session timeline, so two instances on one PC show up exactly like two PCs.

## 6. Changes to the plan

- **S13 (two instances on one PC) runs first**, together with S1.
- **SimClient and SimHost are part of M0.**
- **S11 (Steam relay) is deferred** until a second tester with their own copy is available.
- Dev instance mode exists only in dev builds.
