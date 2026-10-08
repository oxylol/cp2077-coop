# Seamless Co-op for Cyberpunk 2077 — design docs

Design documents for a 2–4 player story co-op mod for Cyberpunk 2077 (base game + Phantom Liberty), built from scratch on RED4ext, redscript, Codeware, ArchiveXL, TweakXL and Valve's open-source GameNetworkingSockets.

> Not affiliated with or endorsed by CD PROJEKT RED. Every player must own the game. No game files or CDPR assets are distributed.

| Doc | Contents |
|---|---|
| [01-architecture.md](01-architecture.md) | Dependencies and repo layout, topology, components, remote-player puppets, transport, authority model and table, ownership handoff, **time fields (shared slow motion)**, replication budget, threading, data flows, failure modes |
| [02-systems.md](02-systems.md) | System-by-system design: quests, scenes, Johnny, combat, quickhacks, cyberware (incl. Sandevistan), scaling, death, loot, police, vehicles, travel, time skip, phone, streaming, special content, saves, UI, mod checks, pause and menus |
| [03-network-protocol.md](03-network-protocol.md) | Lanes, envelope, full message catalog |
| [04-feasibility-and-risks.md](04-feasibility-and-risks.md) | Easy / Hard / Research rating per system, reverse-engineering spikes, legal and project risks |
| [05-local-testing.md](05-local-testing.md) | Testing with one PC and one game copy: two instances side by side, headless simulated players, network impairment |
| [06-roadmap.md](06-roadmap.md) | Milestones M0–M5 with exit criteria checkable on one PC, estimates, spike dependencies |
| [07-testing-guide.md](07-testing-guide.md) | Step by step: putting the project on GitHub, and testing a new build in the game |
| [08-spike-results.md](08-spike-results.md) | What the in-game experiments (spikes) showed so far and what each result decided |
| [09-development-handoff.md](09-development-handoff.md) | Start here to continue development: rules, status, architecture map, build and checks, next steps, gotchas |

Status: **deliverables 1–3 done; M0a delivered and running in the game; M0b portable parts delivered** (network thread, vehicle sync); **M1 time fields delivered on the network side** (Sandevistan/Kerenzikov sync). The game-side parts wait for spike results. See the repository README for what is built and verified.

## Baseline facts (checked 2026-10-07)

- **Target game build: 2.31** (released 2025-09-11; GOG lists the build as 2.31a). It's the latest patch on cyberpunk.net's news feed. RED4ext supports 2.31 with API v1.
- All dependencies are MIT, BSD or Apache licensed ([01 §1.1](01-architecture.md#11-dependencies)).

## Decisions made in these docs

1. **Clean build.** No code from other Cyberpunk multiplayer projects; M0 builds the sync foundation itself.
2. **Star topology**, host runs an in-process HostService (arbiter + relay) inside its game.
3. **One real V per machine**; other players are NPC-based puppets. Never a second `PlayerPuppet`.
4. **Cells + owners**: each machine simulates its own area; shared cells default to the host; explicit handoff protocol with epochs.
5. **Attacker computes offense, victim's owner computes defense.**
6. **Host runs the story; clients' quest execution is frozen** and follows facts; self-contained content (gigs, hustles, side jobs) can be **leased** to a client.
7. **Scenes replay locally on observers** with the host's puppet in V's role; others are never moved, frozen or hidden.
8. **Sandevistan / Kerenzikov create a time field:** the world and every other player in range slow down for everyone, the activator stays at full speed, overlapping activations resolve proportionally. Everything is stamped in field world time so it stays consistent over the network ([01 §8](01-architecture.md#8-time-fields-shared-slow-motion), [02 §6.1](02-systems.md#61-sandevistan-and-kerenzikov)).
9. **Each player sees and hears only their own Johnny** ([02 §3](02-systems.md#3-johnny-silverhand-and-relic-events)).
10. **Menus don't pause the world** unless you're alone ([02 §20](02-systems.md#20-pause-menus-and-presentation-slow-motion)).
11. **Clients never save during a session**; character profile snapshots; return writes a new slot in the client's own save; backups before every session.

## Settled with you

- Sandevistan time field is **local** by default (global is a host setting); slowed players keep **full-speed mouse look**.
- Loot is **instanced per player** by default, "shared, first come" as a host option.
- Test setup: **two PCs, one Steam copy, test on one PC with 16 GB RAM and 12 GB VRAM** → the everyday loop is one game plus the headless tools; two instances only when needed ([05-local-testing.md](05-local-testing.md)).
- License **MIT**; `Cp2077Coop` stays a placeholder name until you pick one.
- Go-ahead given for the roadmap and M0.

## Next

Rounds A–C are done ([08-spike-results.md](08-spike-results.md)): puppets walk (plain NPC body), slow motion is applied in the game, and two games run on one PC. Now: the game side of vehicles, then combat (enemies target puppets through threat injection). In parallel, research on a V body that animates and shows its own player's looks (S3c, S1b/S1c).

## Where to put this

Copy the `docs/` folder into the root of your repository. GitHub renders the Mermaid diagrams natively.
