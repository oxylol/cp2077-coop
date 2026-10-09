#include <shellapi.h>
#include <ArchiveXL/support/red4ext/ArchiveXL.hpp>
#include <TweakXL/support/red4ext/TweakXL.hpp>
#include <App/Settings.h>
#include <RED4ext/LaunchParameters.hpp>

#include "RED4ext/Api/EMainReason.hpp"

std::filesystem::path GCyberpunkMpLocation;

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

void Initialize()
{
    namespace fs = std::filesystem;

    constexpr auto pathLength = MAX_PATH + 1;
    std::string filename;
    do
    {
        filename.resize(filename.size() + pathLength, '\0');

        const auto length = GetModuleFileName((HINSTANCE)&__ImageBase, filename.data(), static_cast<uint32_t>(filename.size()));
        if (length > 0)
        {
            filename.resize(length);
        }
    } while (GetLastError() == ERROR_INSUFFICIENT_BUFFER);

    fs::path path = filename;
    if (is_symlink(path))
    {
        path = read_symlink(path);
    }

    GCyberpunkMpLocation = path.parent_path();
}

#include "App/Application.h"

// The game version this build has actually been tested against. Bump when the mod
// is verified on a newer patch.
constexpr uint8_t kSupportedGameMajor = 2;
constexpr uint16_t kSupportedGameMinor = 31;

RED4EXT_C_EXPORT bool Main(RED4ext::PluginHandle aHandle, RED4ext::EMainReason aReason, const RED4ext::Sdk* aSdk)
{

    switch (aReason)
    {
    case RED4ext::EMainReason::Load:
    {
        // Report the game version, and say plainly when it isn't one we've tested.
        // Without this, an unsupported patch shows up as a wall of script validation
        // errors ("Missing native function ...", "declared base class ... different
        // than current one ...") that give no hint the game version is the problem.
        // Warn rather than refuse: a later patch may well work, and blocking it would
        // be presumptuous.
        if (aSdk->runtime)
        {
            const auto& v = *aSdk->runtime;
            aSdk->logger->InfoF(aHandle, "Game version %u.%u.%u", v.major, v.minor, v.patch);

            if (v.major != kSupportedGameMajor || v.minor != kSupportedGameMinor)
            {
                aSdk->logger->WarnF(aHandle,
                                    "CyberpunkMP was tested against game %u.%u - you are on %u.%u. "
                                    "If the game refuses to start with script validation errors, "
                                    "this mismatch is the likely cause.",
                                    kSupportedGameMajor, kSupportedGameMinor, v.major, v.minor);
            }
        }

        Initialize();

        App::GApplication = MakeUnique<App::Application>(aHandle, aSdk);
        App::GApplication->Bootstrap();

        // Keep the paths alive while their c_str() is in use.
        const auto scriptPath = canonical(GCyberpunkMpLocation / TP_REDSCRIPT_LOCATION);
        aSdk->scripts->Add(aHandle, scriptPath.c_str());

        ArchiveXL::RegisterArchives(canonical(GCyberpunkMpLocation / TP_ARCHIVES_LOCATION));
        TweakXL::RegisterTweaks(canonical(GCyberpunkMpLocation / TP_TWEAKS_LOCATION));

        const auto inputLoaderModule = LoadLibrary("input_loader");
        void(*pInputLoaderAdd)(RED4ext::PluginHandle, const wchar_t*);
        if (inputLoaderModule != nullptr && (pInputLoaderAdd = reinterpret_cast<decltype(pInputLoaderAdd)>(GetProcAddress(inputLoaderModule, "Add"))))
        {
            const auto inputPath = canonical(GCyberpunkMpLocation / TP_INPUTS_LOCATION);
            pInputLoaderAdd(aHandle, inputPath.c_str());
        }
        else
        {
            const auto message =
                L"Cyberpunk Coop needs Input Loader (red4ext\\plugins\\input_loader). It comes with the co-op download: "
                L"extract the whole zip into the game folder again.";
            MessageBoxW(nullptr, message, L"Cyberpunk Coop: Input Loader is missing", MB_SYSTEMMODAL | MB_ICONERROR);
            return false;
        }

        break;
    }
    case RED4ext::EMainReason::Unload:
    {
        App::GApplication->Shutdown();
        App::GApplication = nullptr;
        break;
    }
    }

    return true;
}

RED4EXT_C_EXPORT void Query(RED4ext::PluginInfo* aInfo)
{
    aInfo->name = L"Cyberpunk Coop";
    aInfo->author = L"cp2077-coop, based on CyberpunkMP by Tilted Phoques SRL";
    aInfo->version = RED4EXT_SEMVER(0, 2, 0);

    aInfo->runtime = RED4EXT_V0_RUNTIME_INDEPENDENT;
    aInfo->sdk = RED4EXT_SDK_LATEST;
}

RED4EXT_C_EXPORT uint32_t Supports()
{
    return RED4EXT_API_VERSION_LATEST;
}
