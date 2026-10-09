# Cyberpunk 2077 Seamless Co-op

Story co-op for Cyberpunk 2077 in the spirit of Elden Ring's Seamless Co-op: one player hosts from their own game,
the others join with a password, and everyone sees each other, in and out of cars, while the guests' time of day
and weather follow the host's. There's no server program to run: the host's game is the session.

The character and vehicle sync come from [CyberpunkMP](https://github.com/tiltedphoques/CyberpunkMP) by Tilted
Phoques SRL. This fork keeps only that, runs it inside the host's game, and adds the co-op layer on top. It includes
upstream's fixes for **game patch 2.31**.

## Playing

1. Put the mod into the game folder. *(For now: build it, see below. A single zip with everything it needs is
   coming next: extract it into the game folder and you're done.)*
2. Open `red4ext\plugins\zzzCyberpunkCoop\coop.ini` (it's created on the first start) and set the same `password`
   for everyone, and your `name`. Guests set `join_address` to the host's address.
3. In the game, load a save. The host holds **`/`**: "Hosting a co-op session". Guests hold **`.`**: "Joined …'s
   co-op session". Holding **`/`** again leaves (the host leaving ends the session for everyone).

| | |
|---|---|
| Players | Up to 4 (`max_players`), each from their own save. |
| Shared | The other players (looks, clothes, movement), their cars, the host's time of day (guests only move forward) and weather. |
| Messages | Joins, leaves, wrong password, full session, lost connection: in the middle of the screen. |
| Connecting | For now by address (`join_address`, default `127.0.0.1:11778`: two games on one PC). Over the internet without port forwarding: through Steam, next. |

Two games on one PC: `powershell -ExecutionPolicy Bypass -File tools\coop\start-local.ps1 -Game "<game>\bin\x64\Cyberpunk2077.exe"`
starts one as "Host" and one as "Guest". Don't save in both at once: they share the save folder.

## Build (Windows)

Requirements: Visual Studio 2022 (C++ workload), a Windows SDK **below 10.0.26100** (VS Installer → Individual
components → "Windows 11 SDK (10.0.22621.0)"), [xmake](https://xmake.io) 2.9.9 or newer, git. In the game:
[RED4ext](https://github.com/WopsS/RED4ext/releases), [redscript](https://github.com/jac3km4/redscript/releases),
[Codeware](https://github.com/psiberx/cp2077-codeware/releases), [ArchiveXL](https://github.com/psiberx/cp2077-archive-xl/releases),
[TweakXL](https://github.com/psiberx/cp2077-tweak-xl/releases), [Input Loader](https://github.com/jackhumbert/cyberpunk2077-input-loader/releases).

```powershell
git clone --recursive https://github.com/oxylol/cp2077-coop.git   # not "Download ZIP": it leaves out vendor\
cd cp2077-coop
powershell -ExecutionPolicy Bypass -File tools\check\check-env.ps1   # this PC: VS, Windows SDK, xmake, submodules
xmake f -c -m releasedbg --vs_sdkver=10.0.22621.0 --game="C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077\bin\x64\Cyberpunk2077.exe" -y
xmake check-deps               # the libraries xmake picked: one version of each, none built against others
xmake build Cyberpunk2077      # builds CyberpunkCoop.dll and links it into red4ext\plugins\zzzCyberpunkCoop
xmake build Session.Tests; xmake run Session.Tests   # the co-op session over real connections (also on Linux)
```

In `releasedbg` and `debug` builds the game loads the scripts, tweaks and archives straight from `code/assets`, so
a script change needs no rebuild, only a game restart. GitHub builds and tests everything on every pull request
and every Monday.

## Code

| | |
|---|---|
| `code/server/native` | The co-op session (character and vehicle sync, players, password, host's world state), run by the host's game through `include/HostSession.h`. |
| `code/client` | The mod (RED4ext plugin, `CyberpunkCoop.dll`): Host/Join/Leave (`App/Network/NetworkService.cpp`), settings (`App/Settings.cpp`), puppets, looks, vehicles (`App/World`), time and weather sync (`App/World/WorldSync.cpp`). |
| `code/assets` | Scripts (HUD and hold keys: `redscript/Ink/MultiplayerGameController.reds`), key bindings, tweaks, archives. |
| `code/protocol` | The messages between the games. |
| `code/tests` | The session tests. |

## Next

1. One zip to extract into the game folder, with RED4ext, redscript, Codeware, ArchiveXL, TweakXL and Input Loader.
2. Joining through Steam by password: no addresses, no port forwarding (Steam copies of the game).
3. Shared story progress (quest state) and shared encounters.

## License

CyberpunkMP's license ([LICENSE.md](LICENSE.md)): modifications stay public under the same terms, credit Tilted
Phoques SRL, and are distributed only through GitHub or the authors' own site, never on modding platforms. Not
affiliated with CD PROJEKT RED; every player needs their own copy of the game.
