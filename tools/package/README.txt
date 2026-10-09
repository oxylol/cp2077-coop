Cyberpunk 2077 Seamless Co-op {VERSION}
https://github.com/oxylol/cp2077-coop

Story co-op: one player hosts from their own game, the others join with the same password. You see each
other walk, run, crouch, draw, aim, reload and fire weapons, and sit and drive in cars; the guests follow the
host's time of day and weather. Every player needs their own copy of the game (2.31) and this mod.

INSTALL (everyone)
  Extract the whole zip into the game folder: the folder with bin, engine and r6 in it, e.g.
  C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077
  Replace files when asked. It brings everything the mod needs: RED4ext, redscript, Codeware, ArchiveXL,
  TweakXL and Input Loader (versions and licenses: THIRD_PARTY.txt).
  Updating: extract the new zip the same way, but keep your coop.ini.

SET UP (everyone)
  Open red4ext\plugins\zzzCyberpunkCoop\coop.ini in the game folder:
    password      the same for everyone in the session, one of your own: on Steam, it's how the guests
                  find the host (no addresses, no port forwarding)
    name          yours, shown to the others
    join_address  only without Steam (GOG, Epic): guests set where the host is (coop.ini explains)

PLAY
  Load a save.
  Host:   hold "/"  ->  "Hosting a co-op session"
  Guests: hold "."  ->  "Joined ...'s co-op session"
  Leave:  hold "/"  (when the host leaves, the session ends for everyone)

PROBLEMS
  The mod's log: red4ext\plugins\zzzCyberpunkCoop\CyberpunkCoop.log (a second game on the same PC writes
  CyberpunkCoop-2.log). When the game crashes, where it crashed is at its end ("[crash]" lines).
  RED4ext's logs: red4ext\logs
  Scripts not compiling: r6\logs\redscript_rCURRENT.log
  Report them at https://github.com/oxylol/cp2077-coop/issues

UNINSTALL
  Delete red4ext\plugins\zzzCyberpunkCoop. The requirements are used by many other mods; remove them only if
  nothing else needs them.

Based on CyberpunkMP by Tilted Phoques SRL (LICENSE.md). Not affiliated with CD PROJEKT RED.
