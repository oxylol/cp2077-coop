-- The files are only listed here; the game loads them from code/assets (debug, releasedbg) or from the release zip's
-- assets folder (release; tools/package/make-release.ps1 puts them there).
rule("tweak")
    set_extensions(".tweak")

target("Tweaks")
    -- debug and releasedbg (development) builds load the assets from the repository; release ones from the package.
    if not is_mode("release") then
        add_defines("TP_TWEAKS_LOCATION=\"../../../../code/assets/Tweaks/\"", {public = true})
    else
        add_defines("TP_TWEAKS_LOCATION=\"assets/Tweaks/\"", {public = true})
    end
    set_kind("headeronly")
    set_group("Assets")
    add_rules("tweak")
    add_files("**.tweak")
