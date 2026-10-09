-- xmake check-deps: run after `xmake f`; see check_deps.lua.
task("check-deps")
    set_category("plugin")
    on_run("check_deps")
    set_menu {
        usage = "xmake check-deps",
        description = "Check the resolved packages: one version of each library, no installed library built against other versions.",
        options = {}
    }
