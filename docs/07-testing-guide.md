# 07 — Testing direct-drive puppets in the game (round O)

Remote players are shown as **the game's third-person V**, **placed every frame** at the other player's position, facing their way (no AI routing, so no AI delay), and animated with **the other player's own animation inputs**: the plugin captures what the game feeds your V's animation and sends it along ([01 §4](01-architecture.md#4-remote-players-as-entities)).

Round N: the cutscene lookalike's animation graph is a cutscene graph. It already carries the third-person V animation sets, but nothing in it walks, and lending it your V's (first-person) animation sets changed nothing ([08](08-spike-results.md)). What makes the lookalike look like your V is its impostor (its "copy V" part); what makes the player body walk is its graph with the walking feature. The player body we used, `Character.TPP_Player`, is the female one; the male one exists as its own file, `player_ma_tpp.ent`. Version 0.5.10 can spawn that file directly, so this round tries **the male player body with the lookalike's impostor**.

**This round is T1 and T4l** (10 minutes); the rest of the guide stays for later rounds. **No files to send:** I read the logs and dumps from your game folder; just tell me what you saw.

---

## T0. Before every test session

1. **Back up your saves.** Copy `%USERPROFILE%\Saved Games\CD Projekt Red\Cyberpunk 2077` somewhere safe (paste the path into Explorer's address bar).
2. **Close the game.**
3. **Mods installed and working:** RED4ext, redscript, Codeware, TweakXL and Cyber Engine Tweaks (CET).
4. **Don't save during the tests.** If you must, use a new slot, never an existing one.
5. **With two games running** (T6): both use the same save folder. Never save manually in either one, and treat autosaves made during a two-game session as throwaway.
6. **Delete old results** so you send back only this round's: `probe-results.txt` in the CET mod folder, and `anim-record.txt` / `anim-functions.txt` in `red4ext\plugins\Cp2077Coop` if they exist.

## T1. Update, build, tests

1. Copy the new zip's `cp2077-coop` folder over your project folder (**Replace** when asked).
2. In PowerShell in the project folder:

   ```powershell
   xmake
   xmake run coop-tests
   ```

   The build ends with **`copied N mod file(s) into … (nothing deleted)`**; the tests with **`60 test(s), 0 failure(s)`**. In the game, the top of the **Direct drive** tab shows the version (`Version 0.5.10-m1v`); a `VERSION MISMATCH` line there means the old build is still installed (the new folder wasn't copied over the one you build in).
3. **Your `coop.ini`:** if you made one earlier (`red4ext\plugins\Cp2077Coop\coop.ini`), remove any `[puppet] recordMale/recordFemale` lines from it, so the default body is used. The build installs the new template as `coop.ini.example` next to it; its `[puppet]` and `[anim]` settings all default to the right values for this round (empty `…Input=` lines left over from an older copy now mean "the default").

## T2. Placement test: which way of placing a body moves it? (1 minute)

1. Start the game and load a save. Stand somewhere open, flat and quiet outdoors, with ~10 m of free ground in front of you.
2. CET overlay → **Co-op (dev)** → tab **Direct drive** → **Run placement test**. Stand still for about 20 seconds.
3. A third-person V appears 4 m ahead. The test tries every placement method on it, 3 m at a time:

   | Method | What it is |
   |---|---|
   | teleport | the game's teleportation facility (moves cars and V; didn't move NPCs in round F) |
   | transform | Codeware's `SetWorldTransform`: sets the body's position directly |
   | AI teleport | a teleport command through the NPC's own AI |

   teleport and transform each with nothing switched off, with the AI off, and with the AI and the movement component (`moveComponent`) off; AI teleport with everything on (it needs the AI).
4. **The result is in the Log tab**: one line per try, `… after 0.2 s X m, after 1 s Y m (asked 3.00)`, and at the end either `WORKS: <method>` (the panel then uses it for the mirror) or `no method moved the body …`. Watch the body too: does it jump forward, slide, or not move?

If one works, T4 uses it. If none works, still do T3 and send the results.

## T3. Is the plugin capturing your V's animation inputs? (2 minutes)

1. The bottom of the **Direct drive** tab is the animation status. **Expect** a first line like `animation capture: on, 13 capture point(s), N input(s) captured (handler table: as pointer 0/31, as array 30/31 -> array)`, then one line per capture point: six `gamestateMachineGameScriptInterface::SetAnimationParameter…` / `PushAnimationEvent`, one `entEntity::QueueEvent` (with `… not animation` at the end: all the other events it sees), and the six controller ones from last time.
2. Walk, sprint, crouch, jump, draw and holster a weapon for 20 seconds. **Note:**
   - How many capture points, and does **input(s) captured** go up while you move?
   - Which lines get **from you** counts, which only **other** (NPCs), which **unreadable**?
   - The handler table part in brackets.
   - **Most important:** does `entEntity::QueueEvent` show calls at all? It sees every event in the game, so it should count thousands. If it stays at 0, the way the plugin hooks functions doesn't reach script calls, and the next version changes it.
3. Click **List animation functions** (writes `anim-functions.txt` in `red4ext\plugins\Cp2077Coop`).

If it says `0 capture point(s)` or `neither fits, capture off`: send the bracket text and `anim-functions.txt`.

## T4. Mirror test on one game (the most important part)

The mirror is a third-person V placed in front of you every frame with the **Placement** method shown in the tab (the test picks it; you can change it), facing where you face, with **your own** animation inputs applied to it. On one game it shows what a remote player's puppet will do.

### T4a. Placement: does it follow?

1. **Start mirror**. Within a moment a V (it looks like yours) appears 3 m ahead, with its back to you. It keeps that offset in a fixed direction: when you turn, it turns with you but doesn't swing around you. After turning, **Put it in front of me again** moves it back in front.
2. The status line reads `Mirror (<method>): N placements; while you moved it followed X% of frames; now … m off; AI …; your inputs applied: …`.
3. Walk forward slowly, then stop and turn left and right, then walk backwards and sideways. **Note:**
   - Does it move with you, staying 3 m ahead? Smoothly, or in jerks? Does it turn to face your way?
   - **followed X%** (close to 100 is good). Click **Log mirror state** to put the line into `probe-results.txt`.
4. If the placement test said a method needs the movement component off, **Also switch off** shows `moveComponent`; it applies to the mirror the next time you start it, and to puppets in a session.
5. To compare: **Stop mirror**, pick another **Placement**, **Start mirror** again.

### T4b. Animation: does it move its legs?

With **Apply my animation inputs to it** ticked (the default), do each of these for a few seconds and watch the mirror:

| Do | Note |
|---|---|
| walk, run, sprint, stop | legs moving at the right speed, or gliding in one pose? |
| stand still and turn on the spot | does it step around, or spin like a statue? |
| crouch, crouch-walk, stand up | does it crouch? |
| jump; jump onto something; drop off a ledge | does it jump and land? (its height follows yours either way) |
| draw a weapon, aim, reload, holster | arms and weapon? (it holds *your* weapon, known) |
| slide (sprint, then crouch) and dodge (double-tap a direction) | anything? |

Then untick **Apply my animation inputs to it**, walk and run again, and note any difference. Tick it again.

To watch it from the side: **Stop mirror**, set **Ahead (m)** to `0` and **Right (m)** to `2`, **Start mirror**, then look right while walking forward (you strafe; it walks beside you).

### T4c. Record your inputs (10 seconds)

1. Click **Record my animation inputs (10 s)** and, within the next 10 seconds: stand 2 s, walk 2 s, sprint 2 s, crouch 2 s, jump once.
2. The file is `red4ext\plugins\Cp2077Coop\anim-record.txt`: one line per input with readable names and values. Keep it for sending back.

### T4d. Dump the animation setup

Click **Dump animation components (mirror and V)** with the mirror running. It writes `probe-anim-components.txt` in the CET mod folder: the mirror's and your V's animation, movement and AI components with their settings (including which animation graph each uses).

### T4e. Motion inputs (only if the mirror glides)

Some animation graphs read speed, direction and turning from the character's own movement, which a placed body doesn't have. The plugin works these values out from the placements (the same position, velocity and facing that the session sends anyway) and can feed them into graph inputs, if it knows their names.

1. In **Motion inputs**, type candidate names into the fields (one name per field, as written in the graph; leave the others empty) and press **Use these names**. The animation status shows the names and the values it is feeding (`mirror: speed 3.4 m/s, direction 0 deg, …`).
2. Walk and run: does the mirror start to animate, or animate differently?
3. Where names come from: float inputs in `anim-record.txt` whose values follow your speed, and, if you use WolvenKit, the variable list of the animation graph named in `probe-anim-components.txt`. If you have no candidates, skip this; I'll pick some from your files.
4. A set that works can go into `coop.ini` (`[anim] speedInput=…` and so on), so puppets get it in every session.

Click **Stop mirror** when done.

### T4f. Which body's graph takes V's inputs (round I)

1. **Direct drive** tab. **Apply inputs via** = `events`. **Body** = the first one (`TPP_Player_Cutscene_…`). **Start mirror**.
2. **Dump animation graphs (V and mirror)**. The Log tab names the files (`red4ext\plugins\Cp2077Coop\anim-graphs-V.txt` and `anim-graphs-mirror-….txt`).
3. Press **crouch 1**, then **crouch 0**, then **!Jump**. **Note:** does the mirror crouch, stand, jump?
4. Switch **Apply inputs via** to `controller` and press the three buttons again. **Note** any difference.
5. Walk around with **Apply my animation inputs to it** ticked: anything at all (aiming, crouching when you crouch, jumping when you jump)?
6. **Stop mirror**, pick the next **Body**, **Start mirror**, and do 2–5 again. Do this for every body in the list (6). Some may not appear or may look odd (naked, the plain NPC): note it and go on.

Send all `anim-graphs-*.txt` files, `probe-results.txt`, and one line per body: appears? follows? crouches / jumps on the buttons (events / controller)? anything when you move?

### T4g. A body that looks like V and animates (round J)

Stand somewhere open and quiet outdoors. **Direct drive** tab, **Apply inputs via** = `events`, **Apply my animation inputs to it** ticked. The **Motion inputs** fields already hold `speed_horizontal`, `move_direction`, `speed_vertical`, `rotation_speed_yaw` (the plugin uses them from the start; no need to press **Use these names**).

1. **The player body with an impostor.** **Body** = `Character.TPP_Player`. Tick **Copy my look onto it (add an impostor)**, untick **Give it my animation graph**. **Start mirror**.
   - The animation status (bottom of the tab) has a line `body setup: listening; …; bodies prepared N, impostors added N, …`. **Note** that line. If it says `NOT listening (…)`, note the reason in brackets.
   - **Note:** does the mirror look like your V now (body, head, clothes, hair), or still only a neck? Does it hold your weapon when you draw one?
2. **Moving.** Walk, run and sprint forward, stop; walk backwards; strafe left and right; turn on the spot; crouch-walk; jump. **Note per item:** legs moving at the right speed and in the right direction, or gliding, or walking the wrong way?
   - If it walks the wrong way when you strafe or go backwards: in **Direction (deg)** type `-move_direction`, press **Use these names**, and try again.
   - If the legs move too fast or too slow: note it (the speed is in m/s; the graph may expect another scale).
3. **Without the impostor, to compare.** **Stop mirror**, untick **Copy my look onto it**, **Start mirror**: the neck-only body again? Tick it again afterwards.
4. **The lookalike with your graph.** **Stop mirror**. **Body** = `Character.TPP_Player_Cutscene_…`. Tick **Give it my animation graph**. **Start mirror**. The `body setup:` line should count `graphs swapped 1` (or more). **Note:** does it look like V, does it animate when you move, crouch and jump? Does it stand in a T-pose or odd pose?
5. **Side effects.** With the `TPP_Player` mirror running for a minute: anything odd in the game (HUD, minimap markers, the camera, quest messages, NPCs reacting to it as if it were you)?
6. **Stop mirror**. Click **Log mirror state**, then **Dump animation graphs (V and mirror)** once with each of the two bodies running if anything looked wrong.

Send `probe-results.txt`, the newest `red4ext\logs` file, a screenshot of the Direct drive tab while a mirror runs, and one or two lines for each of steps 1–5.

### T4h. Dress the player body in your items (round K)

1. **Direct drive** tab. **Body** = `Character.TPP_Player`. Untick **Copy my look onto it** and **Give it my animation graph**; tick **Dress it in my items when it appears** (on by default). **Start mirror**.
2. Half a second after it appears, the Log tab shows a `dress: GiveItem + AddItemToSlot(<slot>, <item>)` line per item, then `dress the mirror like you: ok -> N item(s) put on, M failed, K skipped`, and 2 s later a `dress check`. **Note:** what does it look like now? Head, face, hair, body, clothes: which are there, which missing?
3. Click **List looks (V and mirror)** while it runs (writes `probe-looks.txt`: every item in your V's and the mirror's slots, and both their component lists).
4. Walk, run, strafe, crouch and jump: does it still animate?
5. If something is missing: click **Dress it like me now** once more and note any change. Then **Stop mirror**, **Body** = `Character.Player_Puppet_Photomode`, **Start mirror**, and do 2–3 again.
6. **The lookalike with your graph** (missed last round): **Stop mirror**. **Body** = `Character.TPP_Player_Cutscene_…`. Untick **Dress it in my items**, tick **Give it my animation graph**, **Start mirror**. **Note:** does it animate when you move, crouch and jump, and does it still look like V?

If the game crashes during step 2, the last `dress:` line in the Log names the item that did it; start again and tell me. The **Item** / **Slot** fields and **Put this item on the mirror** put one item on the running mirror (for trying something the dressing left out).

### T4l. The male player body with the impostor (round O)

The **Body** list now starts with two file paths: `template:…\player_ma_tpp.ent` (the male third-person player body; `wa` if your V is female) and `template:…\player_ma_tpp_reflexion.ent` (the body the game uses for mirror reflections).

1. **Body** = `template:…\player_ma_tpp.ent`. Tick **Copy my look onto it (add an impostor)**; untick **Lend it my animation sets** and **Switch it to third person when it appears**. Under the motion fields: **Also send my walking…** ticked, **Also tell it to animate as third person** unticked. **Start mirror**. (Dressing skips a body with an impostor.)
   - **Note:** male? Your V's face, hair, head, clothes? Walk, run, strafe, crouch, jump: do the legs and torso move right?
2. **Stop mirror**. Untick **Copy my look**, tick **Dress it in my items when it appears**, **Start mirror**. **Note** the same things (this one is dressed instead of copied).
3. **Head test:** **Stop mirror**. In **Also switch off** type `moveComponent, gameTPPRepresentationComponent`, press **Use for mirror and puppets**, **Start mirror** (still dressed). **Note:** does it keep a head now? Then put **Also switch off** back to `moveComponent` and press the button again.
4. **Body** = `template:…\player_ma_tpp_reflexion.ent`, **Start mirror**. **Note:** what is it (looks like you? moves on its own, copies you, or frozen)?
5. Click **Dump animation graphs (V and mirror)** once while the body from step 1 runs.

If the game crashes when one of these spawns, tell me which step.

### T4k. Why the cutscene lookalike doesn't animate (round N)

1. **Direct drive** tab. **Body** = `Character.TPP_Player_Cutscene_…`. Unticked: **Copy my look**, **Lend it my animation sets**, **Switch it to third person when it appears**. (**Dress it in my items** doesn't matter: the lookalike skips it, it copies your V by itself.) **Start mirror**.
2. Click **Dump animation graphs (V and mirror)**. Walk and run a few steps: **note** whether it does anything at all (legs, arms, breathing idle, or frozen in one pose).
3. **Stop mirror**. **Body** = `Character.TPP_Player`, **Start mirror**, **Dump animation graphs (V and mirror)** again, **Stop mirror**.
4. **Body** = the lookalike again. Tick **Lend it my animation sets (NPC bodies; may crash)**. **Start mirror**. Walk, run, crouch, jump. **Note:** does it animate now, partly, or not at all? Does it still look like your V?
5. If the game crashes right after step 4's Start mirror: that's the lent animation sets; just tell me.

### T4j. A third-person player body (round M)

1. **Direct drive** tab. **Body** = `Character.TPP_Player`. Ticked: **Spawn it with my appearance name**, **Switch it to third person when it appears**, **Dress it in my items when it appears**, and (under the motion fields) **Also send my walking…** and **Also tell it to animate as third person**. **Copy my look** unticked. **Start mirror**.
2. The Log shows `tpp: QueueEvent(...)` twice, the dressing, and after ~2.5 s a `tpp check` line (which head it has, and its appearance). **Note:** does it have a head now? Your V's face and hair? A man's body? Headgear? Does the torso look right when you walk, run and stand?
3. If the game crashes right after a `tpp:` line: start again, untick **Switch it to third person when it appears**, and do 1–2 again (then the switch is the problem; tell me).
4. If it looks wrong in some way, compare: untick **Spawn it with my appearance name** (restart the mirror), then separately untick **Also tell it to animate as third person** (live). Note what changes.
5. Click **List looks (V and mirror)** with the mirror running.

### T4i. Running, and the headgear (round L)

1. **Direct drive** tab. **Body** = `Character.TPP_Player`, **Dress it in my items when it appears** ticked, **Copy my look** and **Give it my animation graph** unticked. Under the motion fields, **Also send my walking as V's movement feature (playerLocomotion)** is ticked (the default). **Start mirror**.
2. Walk, run, sprint, stop; walk backwards; strafe left and right; turn on the spot; jump. **Note per item:** legs moving at the right speed and the right way, or still gliding? Anything odd (legs running the wrong way, stuck in a falling pose)?
3. Untick **Also send my walking…**, walk and run again, and tick it again: any difference?
4. **Headgear:** **Stop mirror**. In **Also switch off** type `moveComponent, gameTPPRepresentationComponent`, press **Use for mirror and puppets**, **Start mirror**. **Note:** does the helmet (or hat) show now? Does running still work? Then try `moveComponent, gameTPPRepresentationComponent, gameFPPCameraComponent` the same way. Put it back to `moveComponent` afterwards.

## T5. Fake players: placement in a session (5 minutes)

Fake players don't send animation inputs, so their puppets glide; this checks placement, delay and the fallback without a second game.

1. Session tab → **Host**.
2. In PowerShell in the project folder:

   ```powershell
   .\build\windows\x64\releasedbg\coop-sim.exe client --connect 127.0.0.1:27077 --bots 2 --script follow
   ```

3. **Expect:** `hosting, 3 player(s)`, `Puppets: 2`, a line `body: Character.TPP_Player_Cutscene_Male (picked by your V's body)` (or `_Female`), `movement: direct, placed every frame (method auto; …)`, and per puppet `… off by 0.0x m, … DIRECT (transform): placed N times, AI off, respawns 0`.
4. Walk, run, sprint and stop; the bots follow, and so do their puppets. **Note:** do the puppets stay exactly where the bots are (off by under 0.1 m) and move smoothly?
5. **Fallback:** if transform doesn't move the body, the line changes to `DIRECT (AI teleport): … (transform failed: …)`; if that doesn't either, to `AI WALKING … (fallback 1: …)`: the bridge went back to AI walking (it tries direct drive again after 60 s, and gives up after the second time). Note what you see and click **Log puppet state**. A line `DIRECT: … AI NOT off after 6 tries (…)` means the plugin couldn't switch the AI off; the animation status then has a `puppet AI switch:` line saying why.
6. Fast travel somewhere: after the load, the puppets should be next to you again within a few seconds.
7. **Leave**, then **Ctrl+C** in PowerShell.

## T6. Two games: the real test

Uses lots of memory: set the **Low** preset and **1280×720 windowed** first (both games share the settings), close other programs, and if you can, put the two windows side by side so you can watch one while playing the other.

1. Start the game from Steam and load a save. Start it a second time (`bin\x64\Cyberpunk2077.exe` in the game folder) and load the same save; it becomes dev instance 2 ("V 2") by itself.
2. **Game 1:** **Host**. **Game 2:** **Join** `127.0.0.1:27077`.
3. **Expect:** each game shows the other player as a third-person V (looking like that game's own V, known), standing exactly where the other V is, facing their way. Session tab: `DIRECT (<method>): placed N times, …`.
4. Play in game 1 and watch game 1's V in game 2. Do the T4b list again (walk, run, sprint, stop, turn on the spot, crouch, jump, ledge, weapon, slide). **Note per item:** animates / glides / wrong animation, and whether it lags behind noticeably (it should be about a tenth of a second behind).
5. Swap: play in game 2, watch in game 1.
6. **Sandevistan across games:** in game 1, **Sandevistan x0.25 for 8 s** (Session tab), then watch game 2: its world slows, and game 1's V keeps normal speed there.
7. **Leave** in game 2, then in game 1. Close both games **without saving**.

## T7. Settings to compare (optional)

All in `red4ext\plugins\Cp2077Coop\coop.ini` (copy `coop.ini.example` if you have none); restart the game after a change.

| Setting | Effect |
|---|---|
| `[anim] apply=false` | puppets are placed but get no animation inputs: shows what the inputs add |
| `[anim] capture=false` | your game captures nothing (the others' puppets of you get nothing) |
| `[puppet] place=transform` (or `aiteleport`, `teleport`) | puppets use only that placement method (default `auto`: transform, then AI teleport, then teleport) |
| `[puppet] switchOff=moveComponent` | also switch the movement component off with the AI (only if the placement test says so) |
| `[puppet] recordMale=Character.TPP_Player` + `recordFemale=Character.TPP_Player` | puppets use the player body (V's own graph); with `addImpostor=true` (default) it copies your V's look |
| `[puppet] useVGraph=true` | puppets (and the mirror) get your V's animation graph |
| `[anim] directionInput=-move_direction` | the direction sent the other way round (if puppets walk the wrong way sideways) |
| `[puppet] drive=ai` | puppets walk by AI move commands again (more delay) |
| `[puppet] drive=ai` + `recordMale=Cp2077Coop.Character.RemotePlayer` + `recordFemale=Cp2077Coop.Character.RemotePlayer` | the old plain-NPC puppets, for comparison |

## T8. What to send back

From `<game folder>\red4ext\plugins\Cp2077Coop\`:
- `anim-record.txt` (T4c) and `anim-functions.txt` (T3)

From `<game folder>\bin\x64\plugins\cyber_engine_tweaks\mods\coop-dev\`:
- `probe-results.txt` and `probe-anim-components.txt`

From `<game folder>\red4ext\logs\`:
- the newest log file(s); this time always, because they show the handler table check, which capture points were routed (`animation capture: …`) and any placement errors

Also:
- Your notes from T2–T6: one or two lines per step, and the T4b table filled in.
- Screenshots of the **Direct drive** tab (animation status) while you move, and of the **Session** tab with puppet lines in T5 or T6.
- Only if scripts didn't load (the panel says **SCRIPTS NOT LOADED**): `r6\logs\redscript_rCURRENT.log`.

## If something's off

- **SCRIPTS NOT LOADED** in the Session tab: send `r6\logs\redscript_rCURRENT.log`.
- **The Direct drive tab says the plugin's animation tools aren't reachable:** CET can't see the plugin's native system. Send the newest `red4ext\logs` file.
- **The mirror never appears** (`spawning for 30 s`, then a log line): note your V's body (male/female) and send `probe-results.txt`; the female lookalike doesn't spawn for a male V and the other way round.
- **The game crashes when the mirror spawns with "Copy my look" or "Give it my animation graph" ticked:** start again, untick both, and send the newest `red4ext\logs` file and the crash report (if the game shows one). The plugin changes the body while the game builds it, which is new in 0.5.5.
- **coop-sim says "rejected: … version mismatch":** run `xmake` again; this version speaks protocol 4.
- **The game doesn't start:** check that `bin\x64\cyberpunk2077_addresses.json` exists; verify the game files in Steam if not.

Earlier rounds and what they decided: [08-spike-results.md](08-spike-results.md).
