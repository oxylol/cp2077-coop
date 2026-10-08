# 03 — Network message catalog

Companion to [01-architecture.md](01-architecture.md). Message names here are the ones used throughout the design docs. Field lists are the semantic content; exact wire layouts are fixed in M0/M1 code.

## 1. Envelope and encoding

- **Transport:** `ITransport` over GameNetworkingSockets (direct) or Steam networking sockets (SDR). Both give connection-oriented messages with reliable and unreliable delivery and per-connection lanes.
- **Envelope:** `u16 msgId | u16 flags | payload` (little-endian). `flags`: relayed (forwarded by host); compressed, has-epoch and has-field-time are reserved for M1+.
- **Encoding:** one bit-packed serializer for every message (`src/core/BitStream.hpp`). Each message has a single `Serialize()` used for both writing and reading, with explicit limits on every string and list; definitions live in `src/protocol/Messages.hpp`. There is no code generator.
- **Snapshots (M1):** the same serializer plus per-receiver delta baselines.
- **Names:** fact names, TweakDB ids, scene paths sent as 64-bit hashes; `StringDict(hash, string)` is sent once per session on first use.
- **Time:** story and session messages carry session time `T` (microseconds, host-synchronized, never dilated). Simulated-world messages (snapshots, fire, hits, effects, projectiles) carry `(fieldId, W)`: field world time, which runs slower inside a time field ([01 §8](01-architecture.md#8-time-fields-shared-slow-motion)). Messages about an entity carry `netId` and `epoch`.
- **Routing:** clients address messages to the host, a peer id, or "interested in netId/cell". The host relays on the net thread without engine involvement.

## 2. Lanes

| Lane | Reliability | Priority | Weight | Used for |
|---|---|---|---|---|
| **L0 Control** | Reliable ordered | 0 (highest) | — | Session, auth, clock, authority, facts, journal |
| **L1 Events** | Reliable ordered | 1 | 60 | Combat, quest, scene, world events |
| **L2 State** | Unreliable | 1 | 40 | Snapshots, fire events, detection |
| **L3 Bulk** | Reliable ordered | 2 | — | World baseline, save transfer, manifests (chunked 16 KB) |
| **L4 Diag** | Reliable, droppable | 3 | — | Desync hashes, log pulls |

[VERIFY] the GameNetworkingSockets version pulled by xmake supports `ConfigureConnectionLanes` (added in GNS 1.4.0). If not, emulate lanes with separate send queues and our own scheduler.

Direction key: **C→H** client to host, **H→C** host to one client, **H→A** host to all, **O→I** owner to interested machines (relayed via host), **P→P** player to player via host.

## 3. Session (0x00xx)

| ID | Message | Dir | Lane | Fields | Rate |
|---|---|---|---|---|---|
| 0x0001 | `Hello` | C→H | L0 | protocolVersion, modVersion, gameBuild (exe file version), exeSize, manifestHash (zero until M5), clientId (persistent UUID), displayName | Once |
| 0x0002 | `Challenge` | H→C | L0 | salt, nonce, passwordRequired, PBKDF2 iterations | Once |
| 0x0003 | `AuthResponse` | C→H | L0 | proof = HMAC-SHA256(PBKDF2(password, salt), nonce ‖ clientId) | Once |
| 0x0004 | `Reject` | H→C | L0 | reason (version, manifest, password, full, banned), detail | Once |
| 0x0005 | `ManifestRequest` / `ManifestData` | H↔C | L3 | full manifest list for diff | On mismatch |
| 0x0006 | `JoinAccept` | H→C | L0 | worldId, peerId, sessionSettings, roster, joinMode (A/C) | Once |
| 0x0007 | `WorldBaseline*` (Begin/Chunk/End) | H→C | L3 | facts, journal, WDL, clock, weather, leases, cell map; optional host save blob (mode C) | Once per join |
| 0x0008 | `ClientReady` | C→H | L0 | spawnTransform, interest | Once |
| 0x0009 | `PlayerJoined` | H→A | L0 | peerId, displayName (appearance follows as `PlayerAppearance`) | Event |
| 0x000E | `PlayerLeft` | H→A | L0 | peerId, reason | Event |
| 0x000A | `SessionSettings` | H→A | L0 | all host settings | On change |
| 0x000B | `RosterState` | H→A | L0 | per peer: state (Playing/Loading/Downed/InScene), RTT | 1 Hz |
| 0x000C | `Kick` | H→C | L0 | reason | Event |
| 0x000D | `Heartbeat` | both | L0 | seq | 2 Hz (also during loads) |

## 4. Clock and world (0x01xx)

| ID | Message | Dir | Lane | Fields | Rate |
|---|---|---|---|---|---|
| 0x0101 | `TimeSyncPing` | C→H | L2 | client local send time | 10 Hz burst after joining, then 1 Hz |
| 0x010C | `TimeSyncPong` | H→C | L2 | client send time, host session time | Reply to each ping |
| 0x0102 | `WorldClock` | H→A | L0 | gameTimeSeconds, timeScale | 1 Hz + on change |
| 0x0103 | `Weather` | H→A | L0 | weatherId, blendSeconds, priority | On change |
| 0x0104 | `StringDict` | any→all | L0 | hash, string | First use |
| 0x0105 | `WorldReset` | H→A | L0 | reason (checkpoint, ending), baseline follows on L3 | Event |
| 0x0106 | `TimeFieldActivate` | activator→H→A | L0 | activationId (activator << 24 \| counter), kind (sandevistan, kerenzikov, scripted), scale in 1/10000 units, T_start, T_end, ease-in/out in ms. Integer units, so the activator computes with exactly what others receive. Host refuses forged ids, scale < 0.05, duration > 30 s, starts more than 2 s off, and tells the sender to end it | Event |
| 0x0107 | `TimeFieldDeactivate` | activator (or H on disconnect)→H→A | L0 | activationId, T_end (can only move earlier) | Event |
| 0x0108 | `TimeFieldMembership` | H→A | L0 | global flag, per player: group id, T of their last group change (starts the 300 ms crossfade) | On change, re-evaluated at 2 Hz |
| 0x0109 | `TimeFieldState` | — | — | **Not needed (as built):** every machine knows every activation and every player's group, and integrates its own world time with a one-second window for late information | — |
| 0x010A | `ClockOffsets` | — | — | **Not needed (as built):** `PlayerState` carries both session time and world time (`fieldId`, `W`) whenever they differ, so a receiver reads each sender's offset from its stream | — |
| 0x010B | `PauseState` | P→H | L0 | paused (solo only), T | On change |

## 5. Interest, entities, authority (0x02xx)

| ID | Message | Dir | Lane | Fields | Rate |
|---|---|---|---|---|---|
| 0x0201 | `InterestUpdate` | C→H | L0 | position, cell set (compressed), inVehicle | 2 Hz + on cell change |
| 0x0202 | `CellOwnership` | H→A | L0 | changed (cellId → peerId) | On change |
| 0x0203 | `EntitySpawn` | O→I | L1 | netId, identityClass, key, record, appearance, transform, initial state, owner, epoch | Event |
| 0x0204 | `EntityDespawn` | O→I | L1 | netId, reason (despawn, dead-cleanup, unloaded) | Event |
| 0x0205 | `EntitySnapshot` | O→I | L2 | per entity: netId, baseline ack, quantized transform, velocity, anim state, flags | 2–20 Hz by relevance |
| 0x0206 | `SnapshotAck` | I→O | L2 | last received snapshot seq per sender | With every packet |
| 0x0207 | `EntityEvent` | O→I | L1 | netId, kind (AI state change, workspot enter/exit, equip, ragdoll start, corpse settle), payload | Event |
| 0x0208 | `AuthorityTransferBegin` | H→old | L0 | netId, toPeer, newEpoch | Event |
| 0x0209 | `AuthorityTransferState` | old→H→new | L1 | netId, newEpoch, fullState, simTime | Event |
| 0x020A | `AuthorityTransferAck` | new→H | L0 | netId, newEpoch | Event |
| 0x020B | `AuthorityTransferCommit` | H→old | L0 | netId, newEpoch | Event |
| 0x020C | `AuthorityTransferAbort` | H→old,new | L0 | netId, epoch, reason | Event |
| 0x020D | `OwnerChanged` | H→A | L0 | netId, owner, epoch | Event |
| 0x020E | `EntityLost` | O→H | L0 | netId, lastState | Event |
| 0x020F | `EntityStateFull` | O→peer | L1 | netId, fullState (desync repair) | On request |

## 6. Players (0x03xx)

| ID | Message | Dir | Lane | Fields | Rate |
|---|---|---|---|---|---|
| 0x0301 | `PlayerState` | P→P | L2 | transform, velocity, aim yaw/pitch, locomotion + stance, weapon state, melee/ability move id + time, vehicle seat, individual time rate ρ, in-menu flag; stamped (fieldId, W) | 30 Hz (60 Hz in close combat / Sandevistan) |
| 0x0302 | `PlayerAppearance` | P→P | L1 | CC state blob, body gender, voice variant | Join + on change |
| 0x0303 | `PlayerAnim` | P→H→P | L1 | sender, session time, full-set flag, up to 256 animation inputs: kind (feature, float, int, bool, vector, event), name hash, then the value, or for a feature its class hash and up to 64 property values (float, int, bool, name, vector). As built (protocol 4) | ≤ 15 Hz, changed inputs only, every event exactly once; full set every 2 s |
| 0x0304 | `PlayerVitals` | P→P | L1 | health %, armor, status-effect icon set, level, heat stage | On change, ≤ 4 Hz |
| 0x0305 | `PlayerAbility` | P→P | L1 | ability (camo, berserk, …), on/off, duration, params. Sandevistan/Kerenzikov go through `TimeFieldActivate` instead | Event |
| 0x0306 | `PlayerDowned` / `PlayerRevived` / `PlayerRespawned` | P→A | L1 | bleedOutEnd, reviver, respawn location | Event |
| 0x0307 | `ReviveStart` / `ReviveCancel` | P→P | L1 | reviver, target | Event |
| 0x0308 | `Ping` | P→A | L1 | world position, kind (go here, enemy, loot) | Event |
| 0x0309 | `Chat` | P→A | L1 | text | Event |
| 0x030A | `PlayerEquipment` | P→P | L1 | equipped items (record + appearance), cyberware visuals, clothing (planned; was 0x0303) | On change |
| 0x0310 | `VehicleState` | owner→H→A | L2 | netId, epoch, seq, session time, position, rotation (smallest-three quaternion, 47 bits), velocity, steer, throttle, brake, flags (lights, horn, handbrake, siren, destroyed); 44 bytes | 30 Hz per driven vehicle |

## 7. Combat (0x04xx)

| ID | Message | Dir | Lane | Fields | Rate |
|---|---|---|---|---|---|
| 0x0401 | `WeaponFire` | shooter→I | L2 | weaponId, muzzle, dir, spread seed, shotIndex; last 3 shots repeated | Per shot |
| 0x0402 | `HitRequest` | shooter→owner | L1 | target netId, epoch, seenAtW (field world time), hitZone, hitPos, offense (damage by type, crit, attack type, flags, status payloads), attackId | Per hit |
| 0x0403 | `HitResult` | owner→I | L1 | attackId, target, applied damage, new health %, effects applied, killed, killer | Per hit |
| 0x0404 | `PlayerHitRequest` | owner→victim | L1 | attacker netId, offense, hitZone, direction | Per hit |
| 0x0405 | `AreaEffect` | origin→I | L1 | center, radius, effect record, instigator, offense, falloff | Event |
| 0x0406 | `StatusEffectApply` / `StatusEffectRemove` | owner→I | L1 | netId, record, remaining, stacks, instigator | Event |
| 0x0407 | `NpcDeath` | owner→I | L1 | netId, killer, contributors[], death type (lethal/non-lethal), ragdoll impulse | Event |
| 0x0408 | `Detection` | owner→player | L2 | npc netId, level, state (unaware/suspicious/alerted/combat) | 10 Hz while > 0 |
| 0x0409 | `TakedownRequest` / `TakedownGrant` / `TakedownDeny` | P↔owner | L1 | target, kind (grapple, takedown, finisher) | Event |
| 0x040A | `PairedAnim` | P→I | L1 | attacker, victim, animId, startTime | Event |
| 0x040B | `QuickhackUpload` | hacker→owner | L1 | target, hack record, upload duration | Event |
| 0x040C | `QuickhackApply` | hacker→owner | L1 | target, hack record, offense / strength, duration | Event |
| 0x040D | `QuickhackCancel` | hacker→owner | L1 | target, reason | Event |
| 0x040E | `BreachResult` | hacker→owners | L1 | access point / target, daemons[] | Event |
| 0x040F | `ProjectileSpawn` / `ProjectileDetonate` | thrower→I | L1 | netId, record, transform, velocity / detonation point | Event |

## 8. Quests and story (0x05xx)

| ID | Message | Dir | Lane | Fields | Rate |
|---|---|---|---|---|---|
| 0x0501 | `FactDelta` | H→A (or lessee→H) | L0 | seq, leaseId?, entries (name hash, int value) | Batched per tick |
| 0x0502 | `JournalDelta` | H→A | L0 | seq, entries (path hash, state, tracked, counters) | Batched |
| 0x0503 | `MapPinUpdate` | H→A | L0 | pin id, kind, position, visibility | On change |
| 0x0504 | `QuestTriggerReport` | C→H | L1 | trigger kind, trigger id, entity netId, player | Event |
| 0x0505 | `QuestLeaseRequest` | C→H | L1 | questId | Event |
| 0x0506 | `QuestLeaseGrant` / `QuestLeaseDeny` | H→C | L1 | questId, leaseId, fact snapshot / reason | Event |
| 0x0507 | `QuestLeaseComplete` | C→H | L1 | leaseId, final facts, journal state | Event |
| 0x0508 | `QuestLeaseRevoke` | H→C | L1 | leaseId, reason | Event |
| 0x0509 | `RewardGrant` | H(or lessee)→P | L1 | money, xp, street cred, items[] | Event |
| 0x050A | `PartyNotification` | H→A | L1 | kind (message, call, quest update), text keys, preview | Event |

## 9. Scenes and dialogue (0x06xx)

| ID | Message | Dir | Lane | Fields | Rate |
|---|---|---|---|---|---|
| 0x0601 | `SceneStart` | runner→I | L1 | scene resource hash, instanceId, actor bindings (role → netId), protagonist peer, V voice variant, startTime, privacy volume id | Event |
| 0x0602 | `SceneSync` | runner→I | L1 | instanceId, section id, section time | 2 Hz |
| 0x0603 | `SceneEnd` | runner→I | L1 | instanceId, reason | Event |
| 0x0604 | `ChoiceHubShow` | runner→I | L1 | instanceId, options (loc key, flags, availability), timeout | Event |
| 0x0605 | `ChoiceSelected` | runner→I | L1 | instanceId, option index | Event |
| 0x0606 | `VOEvent` | runner→I | L1 | line hash, speaker netId, positional | Event (fallback mode + holocalls) |
| 0x0607 | `PerformerAnim` | runner→I | L2 | per performer: workspot/anim id + time, look-at | 20 Hz (fallback mode only) |
| 0x0608 | `PrivacyState` | runner→A | L1 | volume id, state (waiting, locked, released) | Event |
| 0x0609 | `HolocallState` | runner→I | L1 | caller id, active, shared audio on/off | Event |

## 10. World state, loot, police, vehicles, time skip (0x07xx–0x08xx)

| ID | Message | Dir | Lane | Fields | Rate |
|---|---|---|---|---|---|
| 0x0701 | `WorldDelta` | owner→H→A | L1 | entity key, kind, payload, gameTime, author; host assigns seq | Event |
| 0x0702 | `ContainerOpened` | P→H | L1 | container key, player | Event |
| 0x0703 | `ItemDrop` / `ItemPickup` / `ItemPickupDenied` | P↔owner | L1 | item netId, item id (+seed, mods), transform | Event |
| 0x0801 | `CrimeWitnessed` | witness owner→perp | L1 | perpetrator, crime type, witnesses[], position | Event |
| 0x0802 | `HeatUpdate` | P→A | L1 | stage, last known position | On change |
| 0x0803 | `VehicleSpawn` | P→H→A | L1 | netId (spawner << 24 \| counter), vehicle record (TweakDBID), appearance, position, rotation | Event |
| 0x0804 | `VehicleSeatRequest` | C→H | L1 | netId, seat (0 = driver, 0xFF = get out) | Event |
| 0x0808 | `VehicleDespawn` | P→H→A | L1 | netId, reason (dismissed, spawner left, destroyed) | Event |
| 0x0809 | `VehicleSeatState` | H→A | L1 | netId, owner (simulating machine), epoch, seats[] (seat, peer) — complete state, sent on every change | Event |
| 0x0805 | `SkipRequest` | P→H | L1 | target game time | Event |
| 0x0806 | `SkipVote` | H→A, P→H | L1 | request id, vote | Event |
| 0x0807 | `SkipCommit` / `SkipCancelled` | H→A | L0 | new game time / reason | Event |

## 11. Diagnostics (0x0Fxx)

| ID | Message | Dir | Lane | Fields | Rate |
|---|---|---|---|---|---|
| 0x0F01 | `StateHashes` | all→H | L4 | per category digest (owned entities, facts, roster, cells) | 1 Hz |
| 0x0F02 | `DesyncReport` | H→peers | L4 | category, netIds involved | Event |
| 0x0F03 | `LogPull` / `LogChunk` | H↔C | L4 | time window / chunk | On demand |
| 0x0F04 | `NetStats` | all→H | L4 | RTT, jitter, loss, queue depths | 1 Hz |

## 12. Versioning rules

- `protocolVersion` increments on any wire change; mismatches are rejected in `Hello`. Current: **4** (2 vehicles, 3 time fields, 4 animation inputs).
- Any change to a message's `Serialize()` bumps the version; the format has no optional fields.
- Snapshot codec carries its own 8-bit schema id so recorded `.netlog` files remain replayable by the desync viewer.
