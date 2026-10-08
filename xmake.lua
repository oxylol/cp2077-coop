-- Cp2077Coop build (placeholder project name).
--
--   xmake f -m releasedbg            configure (Windows: MSVC x64)
--   xmake                           build everything for this platform
--   xmake run coop-tests            run the portable unit and integration tests
--   xmake run coop-sim host         headless host / fake players (docs/05-local-testing.md)
--   xmake f --game_dir="C:/Games/Cyberpunk 2077" && xmake    also copy the mod into the game (Windows)
--
-- The portable targets (core, protocol, net, host, client, sim, tests) build on Windows and Linux.
-- The game plugin builds on Windows only and needs the vendored SDKs (tools/dev/bootstrap.ps1).

set_project("Cp2077Coop")
set_version("0.4.1")
set_xmakever("2.8.5")

set_languages("cxx20")
add_rules("mode.debug", "mode.release", "mode.releasedbg")
set_defaultmode("releasedbg")
set_warnings("all")

option("game_dir")
    set_default("")
    set_showmenu(true)
    set_description("Cyberpunk 2077 install folder; when set, the plugin build is copied into it")
option_end()

if is_plat("windows") then
    set_arch("x64")
    set_runtimes("MD")
    add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN", "_CRT_SECURE_NO_WARNINGS")
    add_cxflags("/utf-8", "/bigobj", "/permissive-", {tools = "cl"})
end

-- ICE (NAT punching) is off: direct connections in v1, Steam relay later (docs/01-architecture.md §5).
add_requires("gamenetworkingsockets", {configs = {ice = false}})

-- ---------------------------------------------------------------------------------------------------------------------
-- Portable libraries

target("coop_core")
    set_kind("static")
    add_files("src/core/*.cpp")
    add_headerfiles("src/core/*.hpp")
    add_includedirs("src", {public = true})

target("coop_protocol")
    set_kind("static")
    add_deps("coop_core")
    add_files("src/protocol/*.cpp")
    add_headerfiles("src/protocol/*.hpp")

target("coop_net")
    set_kind("static")
    add_deps("coop_protocol")
    add_files("src/net/*.cpp")
    add_headerfiles("src/net/*.hpp")
    add_packages("gamenetworkingsockets", {public = true})

target("coop_host")
    set_kind("static")
    add_deps("coop_net")
    add_files("src/host/*.cpp")
    add_headerfiles("src/host/*.hpp")

target("coop_client")
    set_kind("static")
    add_deps("coop_host") -- SessionRunner hosts too
    add_files("src/client/*.cpp")
    add_headerfiles("src/client/*.hpp")

-- ---------------------------------------------------------------------------------------------------------------------
-- Tools and tests

target("coop-sim")
    set_kind("binary")
    add_deps("coop_host", "coop_client")
    add_files("src/tools/sim/*.cpp")
    add_headerfiles("src/tools/sim/*.hpp")

target("coop-tests")
    set_kind("binary")
    set_default(true)
    add_deps("coop_host", "coop_client")
    add_files("tests/*.cpp", "src/tools/sim/SimPlayer.cpp", "src/plugin/Settings.cpp")
    add_includedirs("tests")

-- ---------------------------------------------------------------------------------------------------------------------
-- Game plugin (Windows)

if is_plat("windows") then
    target("Cp2077Coop")
        set_kind("shared")
        add_deps("coop_host", "coop_client")
        add_files("src/plugin/*.cpp")
        add_headerfiles("src/plugin/*.hpp")
        add_includedirs("vendor/RED4ext.SDK/include", "vendor/RedLib/include", "vendor/RedLib/vendor")
        add_defines("RED4EXT_HEADER_ONLY")
        add_syslinks("user32", "version", "shell32", "ole32")

        -- Assemble the mod layout in build/package, and copy it into the game when game_dir is set.
        after_build(function (target)
            local package = path.join(os.projectdir(), "build", "package")
            os.mkdir(path.join(package, "red4ext", "plugins", "Cp2077Coop"))
            os.cp(target:targetfile(), path.join(package, "red4ext", "plugins", "Cp2077Coop"))
            os.cp(path.join(os.projectdir(), "config", "coop.ini"),
                  path.join(package, "red4ext", "plugins", "Cp2077Coop", "coop.ini.example"))
            os.mkdir(path.join(package, "r6", "scripts", "Cp2077Coop"))
            os.cp(path.join(os.projectdir(), "scripts", "Cp2077Coop", "*.reds"),
                  path.join(package, "r6", "scripts", "Cp2077Coop"))
            os.mkdir(path.join(package, "r6", "tweaks", "Cp2077Coop"))
            os.cp(path.join(os.projectdir(), "tweaks", "Cp2077Coop", "*.yaml"),
                  path.join(package, "r6", "tweaks", "Cp2077Coop"))
            local cet = path.join(package, "bin", "x64", "plugins", "cyber_engine_tweaks", "mods", "coop-dev")
            os.mkdir(cet)
            os.cp(path.join(os.projectdir(), "cet", "coop-dev", "*.lua"), cet)

            -- Never copy folders into the game: xmake's folder copy deletes the existing destination folder
            -- first. install_game copies single files into the mod's own folders only.
            local game = get_config("game_dir")
            if game and game ~= "" then
                import("install_game", {rootdir = path.join(os.projectdir(), "tools", "xmake")})(package, game)
            end
        end)
end
