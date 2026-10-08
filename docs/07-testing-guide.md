# 07 — Step by step: GitHub repository and in-game testing

Two procedures: putting the project on GitHub (once), and testing a new build in the game (every time a new version arrives). Everything assumes Windows, your one Steam copy on patch 2.31, and the project folder you already built from (where you ran `tools\dev\bootstrap.ps1`).

---

## Part A — Put the project on GitHub (once)

You need a GitHub account and Git for Windows (you already have Git: the bootstrap script uses it). Git for Windows includes Git Credential Manager, which handles the GitHub sign-in in your browser.

### A1. Create an empty repository on GitHub

1. On github.com, click **+** (top right) → **New repository**.
2. Name it, e.g. `cp2077-coop`.
3. Choose **Private** or **Public**:
   - **Public:** the automatic builds (GitHub Actions) are free and unlimited.
   - **Private:** you get 2,000 free Actions minutes a month. Windows builds use them up faster than Linux ones, and the very first build is the slowest because it compiles the network library and its dependencies. Later builds reuse a cache.
4. Leave **Add a README**, **.gitignore** and **license** all unticked. The project already has them, and GitHub-made ones would conflict with the first upload.
5. Click **Create repository** and keep the page open. It shows the repository address, e.g. `https://github.com/<your-name>/cp2077-coop.git`.

### A2. Make sure the project folder is up to date

1. Unzip the newest `cp2077-coop-*.zip` I sent.
2. Copy everything inside its `cp2077-coop` folder over your project folder, and choose **Replace** when asked.
3. Your `vendor` folder (the two SDKs) and the hidden `.git` folder stay as they are. The zip doesn't contain them.

Nothing from the game ends up in the repository: no game files are in the project, and `build\`, `.xmake\` and probe results are excluded by `.gitignore`.

### A3. First upload

Open PowerShell in the project folder (in Explorer: click the address bar, type `powershell`, press Enter), then run these one at a time:

```powershell
git config --global user.name "Your Name"          # only needed once per PC
git config --global user.email "you@example.com"   # an address GitHub knows, or your GitHub no-reply address

git status                     # lists the files that will be added
git add -A
git commit -m "Initial import: design docs and M0-M1 code"
git branch -M main
git remote add origin https://github.com/<your-name>/cp2077-coop.git
git push -u origin main        # a browser window asks you to sign in to GitHub the first time
```

**Check:** refresh the repository page. You should see the folders (`docs`, `src`, `tests`, …) and the README.

**About `vendor`:** the bootstrap script added the two SDKs as *submodules*. GitHub stores only a link to the exact SDK commit, not a copy. On the repository page they show as `RED4ext.SDK @ ad72777` and `RedLib @ 6822105`. That's expected.

### A4. Watch the first automatic build

1. On the repository page, open the **Actions** tab. A run named **CI** starts after every push.
2. It has two jobs:
   - **linux:** builds everything except the game plugin and runs the tests.
   - **windows:** builds everything including the plugin, runs the tests, and packages the mod.
3. The first run takes a while (often 20–40 minutes for Windows) because it builds the network library's dependencies. Later runs reuse that work.
4. **Green tick:** everything built and all tests passed.
   **Red cross:** click the job, then the failed step, and send me the error lines.
5. **Downloading a ready-built mod:** open a finished run and scroll to **Artifacts**. **Cp2077Coop-mod** is the mod in the game's folder layout, the same files as your local `build\package`. Useful if you're ever on a PC without the build tools.

### A5. Every later update

When I send a new zip:

```powershell
# 1. copy the zip's contents over the project folder (as in A2), then:
git status                       # see what changed
git add -A
git commit -m "M1: <what the update was>"
git push
```

If you ever want to undo an update, the **Commits** page on GitHub shows every version, and `git log` shows them locally.

---

## Part B — Test a new build in the game

Plan about an hour for the full round, about 10 minutes for B1–B3 alone. The order matters: B1–B3 check that the build works; B4 is the experiments that decide the next code; B5 is two games on one PC.

### B0. Before every test session

1. **Back up your saves.** Copy the folder `%USERPROFILE%\Saved Games\CD Projekt Red\Cyberpunk 2077` somewhere safe (paste that path into Explorer's address bar to open it).
2. **Close the game.**
3. **Mods installed and working:** RED4ext, redscript, Codeware, TweakXL and Cyber Engine Tweaks (CET).
4. **Don't save the game during experiments** (B4). If you must, save to a new slot, never over an existing one.
5. **With two games running** (B5): both use the same save folder. Never save manually in either one. Their autosaves land in the same autosave slots, so treat autosaves made during a two-game session as throwaway; the save you loaded stays untouched.

### B1. Update and build

1. Copy the new zip's contents over the project folder (A2).
2. In PowerShell in the project folder:

   ```powershell
   xmake
   ```

   The build ends with **`copied N mod file(s) into … (nothing deleted)`**. If it says nothing about copying, your game folder isn't set. Set it once with:

   ```powershell
   xmake f --game_dir="C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077"
   xmake
   ```

3. Run the tests:

   ```powershell
   xmake run coop-tests
   ```

   **Expect:** `48 test(s), 0 failure(s)` (the number grows with new versions). If Windows asks whether to allow `coop-tests` on networks, allow **Private networks**; the tests talk to themselves over local network connections.

The fake-player tool is at `build\windows\x64\releasedbg\coop-sim.exe`. The commands below assume you're in the project folder.

### B2. Smoke test: the game hosts, fake players join, puppets walk

1. Start the game from Steam and load a save. Stand somewhere open, outdoors.
2. Open the CET overlay (the key you bound) → window **Co-op (dev)** → tab **Session** → **Host**.
3. In PowerShell:

   ```powershell
   .\build\windows\x64\releasedbg\coop-sim.exe client --connect 127.0.0.1:27077 --bots 2 --script follow
   ```

   If Windows asks about network access for Cyberpunk 2077 or coop-sim, allow **Private networks**.
4. **Expect in the panel:**
   - `hosting, 3 player(s)`
   - two lines `Bot1 … at (x, y)` and `Bot2 … at (x, y)`, whose numbers change as you walk around
   - `Puppets spawned: 2`, a line `body: Cp2077Coop.Character.RemotePlayer`, and one line per puppet. While a puppet loads it says `… body not there yet, waiting N s`; once it's there, `off by … m, speed … m/s, move commands …, respawns …`
   - below those, one line per puppet from the entity system: `managed …, spawning …, spawned …`. **Log puppet state** writes all of it to `probe-results.txt`.
5. **Walking puppets.** Two NPCs (the plain NPC body; the V-lookalikes slide, see C1) should walk after the bots (the bots follow you, so the puppets follow you too). Walk, then run, then sprint for a bit, then stop. **Note:**
   - Do they walk, run and sprint with normal animations, or slide, or stand still?
   - Do they keep up? Roughly what does **off by** show while you move, and does **respawns** go up (a respawn is the puppet popping to the right place)?
   - If a line says **NO AI (can't walk)**, or the puppets never move: see "If the puppets don't walk" below.
6. **Loading-screen check:** fast travel somewhere while the bots are connected. After the load the panel should still say `hosting, 3 player(s)`, and the puppets should pop up next to you again within a few seconds.
7. Press **Leave**. Stop coop-sim with **Ctrl+C**.

**To see the V-lookalike instead** (looks like your V, slides): copy `red4ext\plugins\Cp2077Coop\coop.ini.example` to `coop.ini` in the same folder (if you don't have one yet), and add at the end:

```ini
[puppet]
recordMale=Character.TPP_Player_Cutscene_Male
recordFemale=Character.TPP_Player_Cutscene_Female
```

Restart the game. Remove the lines again to go back.

**If something's off:**
- **SCRIPTS NOT LOADED** in the panel: send me `r6\logs\redscript_rCURRENT.log`. This version's scripts use new game functions (AI move commands), so a mistake there shows up in that log.
- **coop-sim says "rejected: … version mismatch":** the game and coop-sim come from different builds. Run `xmake` again; it rebuilds both.
- **The game doesn't start:** check that `bin\x64\cyberpunk2077_addresses.json` exists. Verify the game files in Steam if it doesn't.

### B3. Sandevistan test (now built into the mod)

Slow motion from a session is now applied by the mod itself; the panel's two switches are gone. (`coop.ini` `[time] applyToGame=false` turns it off.)

1. Host again (B2 step 2), and stand somewhere open.
2. Start two bots that follow you; the first one triggers a Sandevistan every 15 s for 6 s:

   ```powershell
   .\build\windows\x64\releasedbg\coop-sim.exe client --connect 127.0.0.1:27077 --bots 2 --script follow --sandevistan 0.25,6,15
   ```

3. **When Bot1 activates:** your world and you slow to x0.25 for 6 s. The Session tab shows `time fields: world slowed to x0.25`, and Bot1's puppet line shows `own time rate x1.00`: Bot1's puppet keeps moving at normal speed while Bot2's puppet is slow. (Easiest to see if you sprint just before, so both puppets are running.)
4. **Your own:** click **Sandevistan x0.25 for 8 s**. The world and both puppets slow down; you keep full speed (`you exempt`).
5. **Leaving mid-Sandevistan:** click it again and press **Leave** while it runs. Time must return to normal at once.
6. **Ctrl+C** the bots.

### B4. The experiments, round E

Rounds A–D are done ([results](08-spike-results.md)). All in the panel's **Spike probes** tab → **Round E**. Delete `probe-results.txt` before you start.

**E1. S3d: moving a body with less delay** (most important)
1. Set **Record** to `Cp2077Coop.Character.RemotePlayer` (the plain NPC body), **Spawn record in front of V**, step back ~5 m.
2. **AI teleport 5 m**. After 1 s the Log tab says how far it moved. **Note:** did it jump 5 m at once?
3. **AI off, move by teleport every frame**. After 1 s it is moved 12 m forward over 4 s. **Note:** did it move? Smoothly or in jerks? Legs walking, or gliding in one pose? (The Log tab says how far it got; its AI is switched back on afterwards.)
4. **Follow me with matched speed**, then walk, run, sprint, stop and turn around. **Note:** how closely does it keep up, compared with the puppets in a session? Then **Stop following**.
5. **Delete probe NPCs and cars**.

**E2. S2d: a puppet that doesn't fight on its own**
1. Near a gang, spawn the plain NPC body (Record as in E1), then **Make probes passive (senses off)**.
2. Crosshair on a gang member → **S2b** section → **Diagnose** (can the probe still be seen?).
3. Start a fight with the gang. **Note:** does the probe still join the fight on its own? Do gang members still shoot at it?
4. **Delete probe NPCs and cars**.

**E3. S1c: can the lookalike stop copying your weapon?**
1. Set **Record** to `Character.TPP_Player_Cutscene_Male` (`…_Female` for a female V), **Spawn record in front of V**.
2. **List impostor settings** (writes a line to the Log tab), then **Impostor: don't copy weapons**.
3. Switch V's weapon. **Note:** does the lookalike still switch along?
4. Also tell me what happened in round D after **Switch the lookalike's impostor off** (did it stop copying your jacket and weapon?).

### B5. Two games on one PC (new; S13 showed it works)

Uses lots of memory: set the **Low** preset and **1280×720 windowed** in the game's settings first (both games share the settings), and close other programs.

1. Start the game from Steam and load a save. Then start it a second time (run `bin\x64\Cyberpunk2077.exe` from the game folder) and load the same save there.
   The second game becomes **dev instance 2** on its own: its player is called "V 2" and has its own id (the RED4ext log says `dev instance 2`).
2. **Game 1:** CET overlay → **Co-op (dev)** → **Host**.
3. **Game 2:** **Join** with the address `127.0.0.1:27077`.
4. **Expect:** game 1's panel says `hosting, 2 player(s)`; game 2's says it joined. Each game shows the other V as a walking NPC (the plain body, see C1).
5. Walk around in game 1, then switch to game 2 (Alt+Tab) and look. **Note:**
   - Does the other player's puppet walk and run after them? How far behind (`off by … m`)?
   - Does the game you aren't using keep running in the background, or does it pause?
6. **Sandevistan across games:** in game 1, click **Sandevistan x0.25 for 8 s**, then switch to game 2 quickly. **Note:** is game 2's world slowed, and does game 1's puppet keep normal speed there?
7. **Leave** in game 2, then in game 1. Close both games **without saving**.

### B6. What to send back

From `<game folder>\bin\x64\plugins\cyber_engine_tweaks\mods\coop-dev\`:
- `probe-results.txt`
- `probe-s2b.txt` (if you used Diagnose)

Also:
- Your notes from B2–B5: one or two lines per step are plenty.
- A screenshot of the Session tab while the puppets walk after you (with the puppet lines visible).
- Only if something failed: the newest file in `red4ext\logs\`, plus `r6\logs\redscript_rCURRENT.log` if scripts didn't load.

Earlier rounds and what they decided: [08-spike-results.md](08-spike-results.md).
