-- The files are only listed here; the game loads them from code/assets (debug, releasedbg) or from the release zip's
-- assets folder (release; tools/package/make-release.ps1 puts them there).
rule("redscript")
    set_extensions(".reds")

target("redscript")
    -- debug and releasedbg (development) builds load the assets from the repository; release ones from the package.
    if not is_mode("release") then
        add_defines("TP_REDSCRIPT_LOCATION=\"../../../../code/assets/redscript\"", {public = true})
    else
        add_defines("TP_REDSCRIPT_LOCATION=\"assets/redscript\"", {public = true})
    end
    set_kind("headeronly")
    set_group("Assets")
    add_rules("redscript")
    add_files("**.reds")
