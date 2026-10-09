-- The co-op session (character and vehicle sync), linked into the client: the host's game runs it (HostSession.h).
target("Server.Core")
    set_kind("static")
    set_group("Server")
    add_files("**.cpp")

    add_headerfiles("**.h", "**.hpp", "**.inl")
    set_pcxxheader("ServerPCH.h")
    add_includedirs(
        ".",
        "../../../build",
        "../../../vendor"
    )
    -- The client sees HostSession.h only, not the server's headers (both sides have a Game/ folder, for one).
    add_includedirs("include", {public = true})

    add_defines("STEAMNETWORKINGSOCKETS_STATIC_LINK")

    add_deps("Common", "Protocol")

    add_packages(
        "gamenetworkingsockets",
        "mimalloc",
        "spdlog",
        "hopscotch-map",
        "glm",
        "flecs",
        "entt",
        "microsoft-gsl")
