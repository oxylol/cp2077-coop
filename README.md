# Cyberpunk 2077 Seamless Co-op (built on CyberpunkMP)

Story co-op for Cyberpunk 2077, made as an enhancement of [CyberpunkMP](https://github.com/tiltedphoques/CyberpunkMP)
by Tilted Phoques SRL. CyberpunkMP shows other players in your game (their look, clothes, movement, cars, chat);
this fork adds what story co-op needs on top: one player is the **story host** and everyone else's world follows
theirs, while each of you plays from your own story save.

This repository is a fork with CyberpunkMP's full history (remote `upstream`), so upstream changes can be merged.
It includes upstream's open pull requests that make CyberpunkMP run on **game patch 2.31** (#56–#60: the RTTI name
fix without which the game refuses to start, remote-player position fix, spawn crash guards, pinned build).

## What the co-op layer does (so far)

| | |
|---|---|
| Story host | The first player to connect. If they leave, the longest-connected player takes over; `/makehost` takes it. |
| Shared world | Every 2 s the host's game time, weather and tracked quest go to the others. Guests' clocks only move **forward** to the host's time of day (going back breaks quest timers); the weather blends to the host's. |
| Meeting up | `/tp` puts you next to the host, `/tp <name>` next to anyone. |
| Player names | `--name=<name>` on the game's command line (two games on one PC need different names). |
| Others' looks | Each game sends its V's character customization; 2.31 moved where it lives, so the client now searches for it and logs what it found (`[Customization]` in `red4ext/logs`). A player whose look can't be read is still shown dressed with a default head instead of invisible. |

Chat commands (`;` opens the chat): `/help`, `/host`, `/players`, `/tp [name]`, `/sync`, `/makehost`.

Code: server plugin `code/server/scripting/CoopSystem/` (C#), client script `code/assets/redscript/Plugins/Coop.reds`,
engine calls `code/client/App/World/CoopNative.cpp`, customization lookup `code/client/Game/CustomizationState.cpp`.

## Build (Windows)

Requirements: Visual Studio 2022 (C++ workload), a Windows SDK **below 10.0.26100** (VS Installer → Individual
components → "Windows 11 SDK (10.0.22621.0)"), [xmake](https://xmake.io) 2.9.9 or newer, git, .NET 9 SDK and
.NET 8 runtime.
(Building the server on Linux needs the .NET 10 SDK instead: CppSharp's Linux build targets it.)

```powershell
git clone --recursive https://github.com/oxylol/cp2077-coop.git   # not "Download ZIP": it leaves out vendor\
cd cp2077-coop
powershell -ExecutionPolicy Bypass -File tools\check\check-env.ps1   # this PC: VS, Windows SDK, xmake, .NET, submodules
xmake f -c -m releasedbg --vs_sdkver=10.0.22621.0 --game="C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077\bin\x64\Cyberpunk2077.exe" -y
xmake check-deps               # the libraries xmake picked: one version of each, none built against others
xmake build Server.Loader      # server + plugins (CoopSystem, JobSystem)
xmake build Cyberpunk2077      # client; links CyberpunkMP.dll into red4ext\plugins\zzzCyberpunkMP
```

The two checks stop with what to fix (a ZIP download, a too-new Windows SDK, two protobuf versions, a library
built against other versions…) before a long build fails halfway. GitHub runs both, and the whole build, on every
pull request and every Monday.

`releasedbg` is what upstream's CI builds; `-c` throws away an earlier configuration (e.g. a failed `debug` one).
The first run builds the libraries (protobuf, abseil, GameNetworkingSockets…) and takes a while. In `releasedbg`
and `debug` builds the game loads the scripts, tweaks and archives straight from `code/assets`, so a script change
needs no rebuild, only a game restart. `release` builds load them from the installed package.

Game mods required: [RED4ext](https://github.com/WopsS/RED4ext/releases),
[redscript](https://github.com/jac3km4/redscript/releases), [Codeware](https://github.com/psiberx/cp2077-codeware/releases),
[ArchiveXL](https://github.com/psiberx/cp2077-archive-xl/releases), [TweakXL](https://github.com/psiberx/cp2077-tweak-xl/releases),
[Input Loader](https://github.com/jackhumbert/cyberpunk2077-input-loader/releases).

## Test with two games on one PC

```powershell
powershell -ExecutionPolicy Bypass -File tools\coop\start-local.ps1 -Game "<game>\bin\x64\Cyberpunk2077.exe"
```

It starts the server and two games (`--online --ip=127.0.0.1 --port=11778 --name=Host` / `--name=Guest`). In each
game load a save, then **hold `/`** to connect; the first one is the story host. Then check:

1. Each game shows the other player, placed where they are, with their look (or at least dressed).
2. The chat says who joined and who the host is; `/players` lists both.
3. On the guest, the time of day jumps forward to the host's, and the weather follows within a few seconds.
4. `/tp` on the guest puts you next to the host.

Logs: `<game>\red4ext\logs\CyberpunkMP*.log` and the server window. The two games share the save folder, so never
save in both at once.

## Roadmap to a shared story

1. **Now:** see each other, shared time and weather, meet up, host's tracked quest visible (`/host`).
2. **Shared progress:** the host's quest facts and journal state sent to guests, applied when the guest is at the
   same point; guests joining the host's story from a copy of the host's save.
3. **Shared encounters:** NPCs and combat owned by the host's game and mirrored on the guests'.

## License

CyberpunkMP's license ([LICENSE.md](LICENSE.md)): modifications stay public under the same terms, credit Tilted
Phoques SRL, and are distributed only through GitHub or the authors' own site, never on modding platforms. Not
affiliated with CD PROJEKT RED; every player needs their own copy of the game.
