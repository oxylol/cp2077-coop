-- Copies the assembled mod (build/package) into the game folder, one file at a time.
--
-- Rules, because this writes into a game install:
--   * never deletes anything (xmake's folder copy replaces whole destination folders, which once wiped
--     game files and other mods; this module only ever copies single files);
--   * only writes inside the mod's own folders listed below, and refuses anything else;
--   * checks that game_dir really is a Cyberpunk 2077 install first.
--
-- Used by xmake.lua after building the plugin. Can be tested on its own:
--   xmake lua tools/xmake/install_game.lua <package dir> <game dir>

local kModFolders = {
    {"red4ext", "plugins", "Cp2077Coop"},
    {"r6", "scripts", "Cp2077Coop"},
    {"r6", "tweaks", "Cp2077Coop"},
    {"bin", "x64", "plugins", "cyber_engine_tweaks", "mods", "coop-dev"},
}

local function normalize(p)
    return (p:gsub("\\", "/"))
end

local function is_inside_mod_folder(relative)
    relative = normalize(relative)
    for _, parts in ipairs(kModFolders) do
        local prefix = table.concat(parts, "/") .. "/"
        if relative:sub(1, #prefix) == prefix then
            return true
        end
    end
    return false
end

function main(package, game)
    assert(package and game, "usage: install_game <package dir> <game dir>")
    if not os.isfile(path.join(game, "bin", "x64", "Cyberpunk2077.exe")) then
        raise("game_dir does not contain bin/x64/Cyberpunk2077.exe: %s", game)
    end

    -- Check every file before copying any, so a bad package copies nothing.
    local files = {}
    for _, file in ipairs(os.files(path.join(package, "**"))) do
        local relative = path.relative(file, package)
        if not is_inside_mod_folder(relative) then
            raise("refusing to copy %s: it is outside the mod's folders", relative)
        end
        table.insert(files, {source = file, relative = relative})
    end

    for _, entry in ipairs(files) do
        local destination = path.join(game, entry.relative)
        if os.isdir(destination) then
            raise("refusing to overwrite folder %s with a file", destination)
        end
        os.mkdir(path.directory(destination))
        os.cp(entry.source, destination) -- single file onto a file path: overwrites only that file
    end
    cprint("${green}copied %d mod file(s) into %s (nothing deleted)", #files, game)
end
