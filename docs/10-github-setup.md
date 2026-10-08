# 10 — Putting the project on GitHub (once)

Moved here from the testing guide, which now only covers in-game tests ([07](07-testing-guide.md)). Everything assumes Windows and the project folder you already built from (where you ran `tools\dev\bootstrap.ps1`). Every new zip comes with `commit-message.txt`: use it as the commit message (step 5).

You need a GitHub account and Git for Windows (you already have Git: the bootstrap script uses it). Git for Windows includes Git Credential Manager, which handles the GitHub sign-in in your browser.

## 1. Create an empty repository on GitHub

1. On github.com, click **+** (top right) → **New repository**.
2. Name it, e.g. `cp2077-coop`.
3. Choose **Private** or **Public**:
   - **Public:** the automatic builds (GitHub Actions) are free and unlimited.
   - **Private:** you get 2,000 free Actions minutes a month. Windows builds use them up faster than Linux ones, and the very first build is the slowest because it compiles the network library and its dependencies. Later builds reuse a cache.
4. Leave **Add a README**, **.gitignore** and **license** all unticked. The project already has them, and GitHub-made ones would conflict with the first upload.
5. Click **Create repository** and keep the page open. It shows the repository address, e.g. `https://github.com/<your-name>/cp2077-coop.git`.

## 2. Make sure the project folder is up to date

1. Unzip the newest `cp2077-coop-*.zip` I sent.
2. Copy everything inside its `cp2077-coop` folder over your project folder, and choose **Replace** when asked.
3. Your `vendor` folder (the two SDKs) and the hidden `.git` folder stay as they are. The zip doesn't contain them.

Nothing from the game ends up in the repository: no game files are in the project, and `build\`, `.xmake\` and probe results are excluded by `.gitignore`.

## 3. First upload

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

## 4. Watch the first automatic build

1. On the repository page, open the **Actions** tab. A run named **CI** starts after every push.
2. It has two jobs:
   - **linux:** builds everything except the game plugin and runs the tests.
   - **windows:** builds everything including the plugin, runs the tests, and packages the mod.
3. The first run takes a while (often 20–40 minutes for Windows) because it builds the network library's dependencies. Later runs reuse that work.
4. **Green tick:** everything built and all tests passed.
   **Red cross:** click the job, then the failed step, and send me the error lines.
5. **Downloading a ready-built mod:** open a finished run and scroll to **Artifacts**. **Cp2077Coop-mod** is the mod in the game's folder layout, the same files as your local `build\package`. Useful if you're ever on a PC without the build tools.

## 5. Every later update

When I send a new zip:

```powershell
# 1. copy the zip's contents over the project folder (as in step 2), then:
git status                       # see what changed
git add -A
git commit -m "M1: <what the update was>"
git push
```

If you ever want to undo an update, the **Commits** page on GitHub shows every version, and `git log` shows them locally.

**With the commit message from the zip** (recommended; it lists everything the update changed):

```powershell
git add -A
git commit -F "C:\path\to\unzipped\commit-message.txt"
git push
```

`commit-message.txt` sits at the top of the zip, next to the `cp2077-coop` folder. Point `git commit -F` at it where you unzipped it; don't copy it into the project folder, or it gets committed too.
