#pragma once

#include <filesystem>
#include <string>

#include <RED4ext/RED4ext.hpp>

#include "core/Types.hpp"
#include "plugin/Settings.hpp"

namespace coop::plugin
{
// Process-wide plugin context, set up when RED4ext loads the DLL.
struct Environment
{
    RED4ext::v1::PluginHandle handle = nullptr;
    const RED4ext::v1::Sdk* sdk = nullptr;

    std::filesystem::path pluginDir; // red4ext/plugins/Cp2077Coop
    std::filesystem::path gameExe;   // bin/x64/Cyberpunk2077.exe
    std::string gameBuild;           // exe file version, e.g. "3.0.80.51928"
    uint64_t exeSize = 0;

    // Dev instance mode (docs/05-local-testing.md §2): -coopInstance=N on the game's command line, or 2, 3, ...
    // automatically for the second and later games running from the same folder.
    int devInstance = 0;

    Uuid clientId{};
    Settings settings;
};

Environment& Env();

void Initialize(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk);
void Shutdown();
} // namespace coop::plugin
