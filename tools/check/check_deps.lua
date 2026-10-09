-- Checks the project's resolved packages for the mismatches that have broken this build before:
--
-- 1. Two versions of one library. A package recipe may ask for "the newest" of a dependency the project pins to
--    another version (GameNetworkingSockets pulled protobuf 36 and abseil 2026 next to the pinned 29.3 / 20250127.0).
--    Both get linked into one binary: duplicate or missing symbols, or worse, at run time.
-- 2. An installed library built against other versions of its dependencies than the ones in use. xmake reuses an
--    installed package by its own settings only, not its dependencies' versions, so a copy built earlier against
--    other versions is picked up silently (LNK2005/LNK2001 on google::protobuf and absl::lts_* symbols).
-- 3. A precompiled (downloaded) library that has library dependencies. xmake's build-artifacts are built against
--    whatever versions their CI resolved, which xmake neither records nor checks; such packages need build = true.
--
-- Fixes go into the add_requireconfs at the top of the root xmake.lua.

import("core.base.task")
import("core.project.config")
import("core.project.project")
import("private.action.require.impl.package")
import("private.action.require.impl.repository")
import("private.action.require.impl.environment")

local function _parent_names(instance)
    local names = {}
    for _, parent in ipairs(table.wrap(instance:parents())) do
        table.insert(names, parent:name())
    end
    return #names > 0 and table.concat(names, ", ") or "the project"
end

local function _check(problems)
    local requires, requires_extra = project.requires_str()
    if not requires or #requires == 0 then
        return 0
    end
    if not repository.pulled() then
        task.run("repo", {update = true})
    end

    -- every package once, roots and dependencies
    local all = {}
    local seen = {}
    local function add(instance)
        if not seen[instance] then
            seen[instance] = true
            table.insert(all, instance)
        end
    end
    for _, instance in ipairs(package.load_packages(requires, {requires_extra = requires_extra})) do
        add(instance)
        for _, dep in ipairs(instance:orderdeps()) do
            add(dep)
        end
    end

    -- the versions in use: fetching resolves a package found on the system to the system's version
    local function inuse(instance)
        local fetchinfo = instance:fetch()
        return (fetchinfo and fetchinfo.version) or instance:version_str() or "?"
    end

    -- 1. one version per library (host tools such as cmake are left out)
    local versions = {}
    for _, instance in ipairs(all) do
        if instance:is_library() and not instance:is_host() then
            local name = instance:name()
            local version = inuse(instance)
            versions[name] = versions[name] or {}
            versions[name][version] = versions[name][version] or {}
            table.insert(versions[name][version], _parent_names(instance))
        end
    end
    for name, byversion in table.orderpairs(versions) do
        local list = {}
        for version, users in table.orderpairs(byversion) do
            table.insert(list, string.format("%s (for %s)", version, table.concat(users, "; ")))
        end
        if #list > 1 then
            table.insert(problems, string.format("%s is resolved to %d versions: %s. Pin the one the project uses "
                .. "for the others with add_requireconfs(\"<package>.%s\", {override = true, version = ...}).",
                name, #list, table.concat(list, ", "), name))
        end
    end

    -- 2. and 3. installed libraries against the dependencies in use
    for _, instance in ipairs(all) do
        local manifest = instance:is_library() and not instance:is_system() and not instance:is_thirdparty()
                         and instance:manifest_load()
        local librarydeps = manifest and instance:librarydeps() or {}
        if manifest and #librarydeps > 0 then
            local artifacts = manifest.artifacts or {}
            if artifacts.remotedir and artifacts.remotedir ~= artifacts.installdir then
                table.insert(problems, string.format("%s %s is a downloaded precompiled package, built against "
                    .. "unknown versions of %d librar%s. Add build = true to its add_requireconfs and reinstall it.",
                    instance:name(), instance:version_str() or "", #librarydeps, #librarydeps == 1 and "y" or "ies"))
            else
                for _, dep in ipairs(librarydeps) do
                    local recorded = manifest.deps and manifest.deps[dep:name()]
                    local version = inuse(dep)
                    -- For a dependency taken from the system, xmake records the version it asked for, not the
                    -- system's (openssl3 3.6.5 for the system's 3.0.13).
                    if dep:is_system() then
                        version = dep:version_str() or version
                    end
                    if recorded and recorded.version ~= version then
                        table.insert(problems, string.format("%s %s (installed in %s) was built against %s %s, but "
                            .. "the project uses %s %s. Give its build its own identity (a changed config) or "
                            .. "remove that install directory.", instance:name(), instance:version_str() or "",
                            instance:installdir(), dep:name(), recorded.version, dep:name(), version))
                    end
                end
            end
        end
    end
    return #all
end

function main()
    task.run("config", {require = false}, {disable_dump = true})
    os.cd(project.directory())
    environment.enter()

    local problems = {}
    local checked
    local ok = try
    {
        function ()
            checked = _check(problems)
            return true
        end,
        catch
        {
            function (errors)
                cprint("${color.warning}check-deps: could not inspect the packages with this xmake version: %s", tostring(errors))
            end
        }
    }
    environment.leave()
    if not ok then
        raise("check-deps failed to run")
    end

    if #problems > 0 then
        cprint("${color.failure}check-deps: %d problem(s) with the resolved packages:", #problems)
        for _, problem in ipairs(problems) do
            print("  - " .. problem)
        end
        raise("check-deps found problems")
    end
    cprint("${color.success}check-deps: %d packages, one version of each library, none built against other versions", checked)
end
