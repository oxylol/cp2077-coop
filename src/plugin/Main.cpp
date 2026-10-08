// RED4ext entry points for Cp2077Coop.

#include <RED4ext/RED4ext.hpp>
#include <RedLib.hpp>

#include "client/SessionRunner.hpp"
#include "core/Version.hpp"
#include "net/GnsTransport.hpp"
#include "plugin/CoopSystem.hpp"
#include "plugin/Plugin.hpp"
#include "plugin/ScriptTypes.hpp"

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle aHandle, RED4ext::v1::EMainReason aReason,
                                        const RED4ext::v1::Sdk* aSdk)
{
    switch (aReason)
    {
    case RED4ext::v1::EMainReason::Load:
        coop::plugin::Initialize(aHandle, aSdk);
        // Registers CoopSystem (as a game system with a GameInstance.GetCoopSystem() getter) and the
        // script-visible structs.
        Red::TypeInfoRegistrar::RegisterDiscovered();
        break;

    case RED4ext::v1::EMainReason::Unload:
        // The process is exiting: stop the network thread, then leave the network library loaded rather
        // than tearing it down under the game's feet.
        coop::SessionRunner::StopAllThreads();
        coop::GnsTransport::ShutdownLibrary(false);
        coop::plugin::Shutdown();
        break;
    }
    return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* aInfo)
{
    aInfo->name = L"Cp2077Coop";
    aInfo->author = L"Cp2077Coop contributors";
    aInfo->version = RED4EXT_V1_SEMVER(coop::kVersionMajor, coop::kVersionMinor, coop::kVersionPatch);
    // Pinned: RED4ext refuses to load the plugin on any other game build (docs/04 "version pinning").
    aInfo->runtime = RED4EXT_V1_RUNTIME_VERSION_2_31;
    aInfo->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports()
{
    return RED4EXT_API_VERSION_1;
}
