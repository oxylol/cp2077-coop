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

Plan about an hour for the full round, about 10 minutes for B1–B3 alone. The order matters: B1–B3 check that the build works; B4 is the experiments that decide the next code.

### B0. Before every test session

1. **Back up your saves.** Copy the folder `%USERPROFILE%\Saved Games\CD Projekt Red\Cyberpunk 2077` somewhere safe (paste that path into Explorer's address bar to open it).
2. **Close the game.**
3. **Mods installed and working:** RED4ext, redscript, Codeware, TweakXL and Cyber Engine Tweaks (CET).
4. **Don't save the game during experiments** (B4). If you must, save to a new slot, never over an existing one.

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

   **Expect:** `47 test(s), 0 failure(s)` (the number grows with new versions). If Windows asks whether to allow `coop-tests` on networks, allow **Private networks**; the tests talk to themselves over local network connections.

The fake-player tool is at `build\windows\x64\releasedbg\coop-sim.exe`. The commands below assume you're in the project folder.

### B2. Smoke test: the game hosts, fake players join

1. Start the game from Steam and load a save.
2. Open the CET overlay (the key you bound) → window **Co-op (dev)** → tab **Session** → **Host**.
3. In PowerShell:

   ```powershell
   .\build\windows\x64\releasedbg\coop-sim.exe client --connect 127.0.0.1:27077 --bots 2 --script follow
   ```

   If Windows asks about network access for Cyberpunk 2077 or coop-sim, allow **Private networks**.
4. **Expect in the panel:**
   - `hosting, 3 player(s)`
   - two lines `Bot1 … at (x, y)` and `Bot2 … at (x, y)`, whose numbers change as you walk around
   - `Puppets spawned: 2`

   Known for now: the stand-in NPCs appear but don't move. That's what spike S3 is for.
5. **Loading-screen check (new network thread):** fast travel somewhere while the bots are connected. After the load the panel should still say `hosting, 3 player(s)` and the bots should still be connected.
6. Press **Leave**. Stop coop-sim with **Ctrl+C**.

**If something's off:**
- **SCRIPTS NOT LOADED** in the panel: see the redscript log `r6\logs\redscript_rCURRENT.log`.
- **coop-sim says "rejected: … version mismatch":** the game and coop-sim come from different builds. Run `xmake` again; it rebuilds both.
- **The game doesn't start:** check that `bin\x64\cyberpunk2077_addresses.json` exists. Verify the game files in Steam if it doesn't.

### B3. Sandevistan test

1. Host again (B2 step 2), and stand somewhere open.
2. Start two bots that follow you; the first one triggers a Sandevistan every 15 s for 6 s:

   ```powershell
   .\build\windows\x64\releasedbg\coop-sim.exe client --connect 127.0.0.1:27077 --bots 2 --script follow --sandevistan 0.25,6,15
   ```

3. **Expect:**
   - About 3 s after joining, coop-sim prints `[Bot1] Sandevistan x0.25 for 6.0 s`.
   - While it runs, the panel's **Time:** line shows `world x0.25, you x0.25`, and coop-sim shows `Bot1 … time: world x0.25, me x1.00 (Sandevistan)` and `Bot2 … time: world x0.25, me x0.25`.
   - Afterwards everything goes back to `x1.00`.
4. **Your Sandevistan:** click **Sandevistan x0.25 for 8 s** in the panel.
   - The panel shows `world x0.25, you x1.00`.
   - coop-sim shows both bots at `world x0.25, me x0.25`.
5. **Experimental, part of S8:** tick **Apply to the game** and wait for Bot1's next Sandevistan.
   - Does your world slow down? Smoothly? Do you move at normal speed or slowed? Does everything return to normal afterwards?
   - Untick it whenever you like; normal time is restored. If anything fails, it switches itself off and writes the reason to the **Log** tab.
6. **Leave**, **Ctrl+C**.

### B4. The experiments (spikes)

All in the panel's **Spike probes** tab, in a loaded save. Every probe is wrapped so a wrong guess shows an error instead of breaking anything. Results are written to `probe-results.txt` (and `probe-*.txt` dumps). The **Log** tab shows them too. Do them in this order; write down what you see for each.

**S3: can an NPC be walked around? (most important now)**
1. **S1** section: leave **Record** as it is → **Spawn record in front of V**.
2. Walk about 10 m away.
3. **S3** section → **AI-walk probe NPCs to V**.
4. **Note:** does it walk (or run) to you with a normal animation, slide without animating, or not move?
5. **Dump AI and animation types**.

**S1: what can look like V?**
1. **List candidate records** (writes `probe-s1-records.txt`).
2. Copy a few names containing `TPP_Player` from that file into **Record**, then **Spawn record in front of V** for each.
3. **Note:** which ones look like V (body, clothing)?
4. **Dump customization types**.

**S1 vehicles: can we move a car and seat an NPC?**
1. **Spawn vehicle in front of V**.
2. **Move probe vehicle 5 m (teleport)**. After 1 s the Log tab says how far it actually moved.
3. Spawn a probe NPC (S1 section), then **Seat probe NPC as passenger**. After 2 s the Log tab says how far the NPC is from the car. Also look: is it sitting in it?
4. **Dump vehicle types**.

**S2: do enemies treat an NPC as a player?**
1. Go near a gang, spawn a probe NPC, **Make probe NPCs player-aligned**.
2. Start a fight.
3. **Note:** do enemies shoot the probe NPC? Does it fight back?

**S8: slow motion**
1. **World 0.25 for 3 s, V exempt**. **Note:** did the world slow while you moved normally?
2. Spawn a probe NPC, **Probe NPCs individual 2.0 for 3 s**. **Note:** did it speed up?
3. **Dump time types**.
4. Plus your notes from B3 step 5.

**Clean up:** **Delete probe NPCs** (also removes probe vehicles).

**S13: two copies of the game at once (do it last; heavy on 16 GB)**
1. Close other programs.
2. With the game running from Steam, start `bin\x64\Cyberpunk2077.exe` from the game folder a second time.
3. **Note** exactly what happens: nothing, an error, a Steam message, or a second window. Close the second one if it opened.

### B5. What to send back

From `<game folder>\bin\x64\plugins\cyber_engine_tweaks\mods\coop-dev\`:
- `probe-results.txt`
- every `probe-*.txt` file

Also:
- Your notes from B2–B4: one or two lines per step are plenty.
- A screenshot of the Session tab during a bot's Sandevistan.
- Only if something failed: the newest file in `red4ext\logs\`, plus `r6\logs\redscript_rCURRENT.log` if scripts didn't load.

Delete `probe-results.txt` before the next round, so the next results don't mix with these.
