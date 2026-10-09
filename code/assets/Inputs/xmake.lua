-- The files are only listed here; the game loads them from code/assets (debug, releasedbg) or from the release zip's
-- assets folder (release; tools/package/make-release.ps1 puts them there).
rule("input")
    set_extensions(".xml")

target("Inputs")
    -- debug and releasedbg (development) builds load the assets from the repository; release ones from the package.
    if not is_mode("release") then
        add_defines("TP_INPUTS_LOCATION=\"../../../../code/assets/Inputs/CyberpunkMP.xml\"", {public = true})
    else
        add_defines("TP_INPUTS_LOCATION=\"assets/Inputs/CyberpunkMP.xml\"", {public = true})
    end
    set_kind("headeronly")
    set_group("Assets")
    add_rules("input")
    add_files("CyberpunkMP.xml")
