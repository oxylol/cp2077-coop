add_requires("cxxopts")

target("NetPack")
    set_kind("binary")
    add_files("**.cpp", "**.cc")
    set_group("Tools")
    add_headerfiles("**.h", "**.hpp", "**.inl")
    set_policy("build.fence", true) -- dependents wait until it is built (generated code)

    set_pcxxheader("NetPackPCH.h")
    add_includedirs(
        ".", 
        "../../build", 
        "../../vendor"
    )

    add_deps("Common")

    add_packages(
        "mimalloc",
        "spdlog",
        "hopscotch-map",
        "cxxopts",
        "protobuf-cpp",
        "abseil")