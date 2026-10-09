add_requires("minhook", "wil", "nameof", "semver")

target("Client")
    set_basename("CyberpunkCoop")
    set_kind("shared")
    set_group("Client")
    set_symbols("debug", "hidden")
    add_defines("_CRTDBG_MAP_ALLOC")
    add_ldflags("/LARGEADDRESSAWARE")
    add_includedirs(
        ".",
        "../",
        "../../build",
        "../../vendor/")
    set_pcxxheader("stdafx.h")
    add_headerfiles("**.h", "**.hpp", "**.inl")
    add_files("**.cpp")
    add_linkdirs(".")
    add_syslinks(
        "user32",
        "version",
        "shell32",
        "comdlg32",
        "bcrypt",
        "ole32",
        "dxgi",
        "d3d12",
        "gdi32",
        "SetupAPI",
        "Powrprof",
        "Cfgmgr32",
        "Propsys",
        "delayimp")

    add_deps("Common", "Protocol", "Server.Core", "RED4ext.SDK", "redscript", "Archives", "Inputs", "Tweaks")

    add_packages(
        "mimalloc",
        "spdlog",
        "hopscotch-map",
        "gamenetworkingsockets",
        "minhook",
        "mem",
        "glm",
        "nlohmann_json",
        "entt",
        "wil",
        "nameof",
        "semver",
        "microsoft-gsl",
        "flecs")

