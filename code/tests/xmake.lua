-- xmake build Session.Tests && xmake run Session.Tests: the co-op session (code/server/native) with real network
-- clients on the loopback address, all driven from one thread like the game does. Runs on Windows and Linux.
target("Session.Tests")
    set_kind("binary")
    set_group("Tests")
    add_files("*.cpp")
    add_includedirs("../../build", "../../vendor")
    -- The project defines _UNICODE, with which Catch2 2.x picks wmain() on Windows and then doesn't compile.
    add_defines("STEAMNETWORKINGSOCKETS_STATIC_LINK", "DO_NOT_USE_WMAIN")
    add_deps("Common", "Protocol", "Server.Core")
    add_packages(
        "catch2",
        "gamenetworkingsockets",
        "mimalloc",
        "spdlog",
        "hopscotch-map",
        "glm",
        "flecs",
        "entt",
        "microsoft-gsl")
