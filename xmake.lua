-- The package recipes (xmake-repo, always the newest) are written for the newest xmake: with 2.9.9 mimalloc, spdlog,
-- abseil and openssl3 no longer install ("bad argument #1 to 'directory'"). Before 2.9.9, MSVC 14.43+ also became
-- the nonexistent CMake toolset "v144". The workflows use this version.
set_xmakever("3.1.1")
set_policy("build.ccache", false)
set_policy("package.requires_lock", false)

add_cxflags("-fPIC")

-- c code will use c99,
set_languages("c99", "cxx20")
add_configfiles("BuildInfo.h.in")

-- Every dependency is pinned, deliberately.
--
-- Unpinned entries resolve to whatever is newest on the day, so a clean checkout builds
-- against a different set of libraries every time. CI does not notice because it restores
-- a cached package set keyed on hashFiles('**/xmake.lua') - the cache hides the drift, and
-- only a fresh clone finds it. As of August 2026 a fresh clone of main does not build.
--
-- These are the versions a full build was verified against, not guesses. Loosening any of
-- them is fine; doing it by accident is what this prevents.
add_requires(
    "mimalloc 2.1.7",
    "spdlog v1.17.0",
    "hopscotch-map v2.4.0",
    "gamenetworkingsockets v1.6.0",
    "glm 1.0.3",
    "nlohmann_json v3.12.0",
    "flecs v4.0.3",
    -- 3.19.4 lacks RecordError and absl::string_view; 35.1 removed
    -- FieldDescriptor::is_optional() and internal symbols code/netpack/cpp/helpers.h
    -- reaches for. 29.3 is the version this code was written against.
    "protobuf-cpp 29.3",
    "entt v3.16.0",
    "microsoft-gsl v4.2.2")

-- GameNetworkingSockets v1.6.0 is built with CMake in Release unless asked otherwise. In a debug build the packages
-- get the debug runtime (/MDd, which defines _DEBUG), and GNS's headers then stop with "Cannot define both NDEBUG
-- and _DEBUG", so it is built in Debug there. Its recipe also asks for the newest protobuf and abseil, which put a
-- second protobuf (36.x) and abseil next to the pinned ones above; it gets the same versions instead. ICE (P2P
-- connections through NAT) isn't used.
--
-- GNS compiles protobuf's headers into itself, so it only links against the protobuf it was built with. xmake
-- identifies a package build by its own settings, not its dependencies' versions, and would happily use a
-- precompiled GNS (xmake's build-artifacts, built against the newest protobuf) or one built earlier against
-- another protobuf: LNK2005/LNK2001 on google::protobuf and absl::lts_2026... symbols. So GNS is always built here
-- (build = true), and the define, which does nothing else, gives this build its own identity; change it along with
-- the protobuf/abseil versions.
add_requireconfs("gamenetworkingsockets", {build = true, configs = {
    debug = is_mode("debug"), ice = false, cxflags = "-DCYBERPUNKMP_GNS_PROTOBUF_29_3_ABSEIL_20250127_0"}})
add_requireconfs("gamenetworkingsockets.protobuf-cpp", {override = true, version = "29.3"})
add_requireconfs("gamenetworkingsockets.abseil", {override = true, version = "20250127.0"})

if is_plat("windows") then
    set_arch("x64")
    add_cxflags("/bigobj")
    add_defines("NOMINMAX")
end

add_defines("_UNICODE", "RED4EXT_STATIC_LIB", "GLM_ENABLE_EXPERIMENTAL")

set_warnings("all")
add_vectorexts("sse", "sse2", "sse3", "ssse3")

-- build configurations
add_rules("mode.debug", "mode.releasedbg", "mode.release")

if is_mode("debug") then
    add_defines("TP_DEBUG")
    set_symbols("debug", "edit")
end

includes('tools/codegen')
includes('tools/check')

-- add projects
includes("code/netpack")
includes("code/common")
includes("code/protocol")
includes("code/server")
includes("code/tests")

option("game")
    set_showmenu(true)
    set_default("Cyberpunk2077.exe")
    set_description("Set the path to Cyberpunk2077.exe for easy debugging")

if is_plat("windows") then
    includes("code/assets")
    includes("code/client")
    includes("code/loader")
    includes("vendor/")
end
