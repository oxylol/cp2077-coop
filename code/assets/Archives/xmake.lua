-- The files are only listed here; the game loads them from code/assets (debug, releasedbg) or from the release zip's
-- assets folder (release; tools/package/make-release.ps1 puts them there).
rule("archive")
    set_extensions(".archive")

target("Archives")
    -- debug and releasedbg (development) builds load the assets from the repository; release ones from the package.
    if not is_mode("release") then
        add_defines("TP_ARCHIVES_LOCATION=\"../../../../code/assets/Archives/packed/archive/pc/mod/\"", {public = true})
    else
        add_defines("TP_ARCHIVES_LOCATION=\"assets/Archives\"", {public = true})
    end
    set_kind("headeronly")
    set_group("Assets")
    add_rules("archive")
    add_files("packed/archive/pc/mod/CyberpunkMP.archive")
