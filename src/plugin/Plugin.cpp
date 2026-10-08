#include "plugin/Plugin.hpp"

#include <Windows.h>

#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <vector>

#include "core/Crypto.hpp"
#include "core/Log.hpp"
#include "core/Version.hpp"

namespace coop::plugin
{
namespace
{
Environment g_environment;

std::filesystem::path ModulePath(HMODULE aModule)
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(aModule, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size())
        {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::string FileVersion(const std::filesystem::path& aFile)
{
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(aFile.c_str(), &ignored);
    if (size == 0)
        return "unknown";

    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(aFile.c_str(), 0, size, data.data()))
        return "unknown";

    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoSize = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) || !info)
        return "unknown";

    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "."
         + std::to_string(HIWORD(info->dwFileVersionLS)) + "." + std::to_string(LOWORD(info->dwFileVersionLS));
}

int ParseDevInstance()
{
    const wchar_t* commandLine = GetCommandLineW();
    const wchar_t* flag = commandLine ? std::wcsstr(commandLine, L"-coopInstance=") : nullptr;
    if (!flag)
        return 0;
    return static_cast<int>(std::wcstol(flag + std::wcslen(L"-coopInstance="), nullptr, 10));
}

// Holds an exclusive lock file for this process's lifetime. Fails while another running game holds it.
bool TryLockFile(const std::filesystem::path& aFile)
{
    const HANDLE handle = CreateFileW(aFile.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    return handle != INVALID_HANDLE_VALUE; // deliberately never closed: Windows releases it when the game exits
}

// Several games started from the same folder (two-instance testing on one PC): the first one is the normal
// instance, later ones become dev instances 2, 3, ... on their own, so each gets its own player name and id.
int DetectInstance(const std::filesystem::path& aPluginDir)
{
    if (TryLockFile(aPluginDir / "instance.lock"))
        return 0;
    for (int instance = 2; instance <= 8; ++instance)
    {
        if (TryLockFile(aPluginDir / ("instance-" + std::to_string(instance) + ".lock")))
            return instance;
    }
    return 0;
}

// A random id per installation (and per dev instance), kept next to the plugin.
Uuid LoadOrCreateClientId(const std::filesystem::path& aFile)
{
    Uuid id{};
    std::ifstream in(aFile);
    std::string hex;
    if (in >> hex && hex.size() == id.size() * 2)
    {
        bool ok = true;
        for (size_t i = 0; i < id.size() && ok; ++i)
        {
            const auto byte = hex.substr(i * 2, 2);
            char* end = nullptr;
            id[i] = static_cast<uint8_t>(std::strtoul(byte.c_str(), &end, 16));
            ok = end && *end == '\0';
        }
        if (ok)
            return id;
    }

    crypto::RandomBytes(id.data(), id.size());
    std::ofstream out(aFile, std::ios::trunc);
    out << crypto::ToHex(id.data(), id.size()) << "\n";
    return id;
}

void LogToRed4ext(LogLevel aLevel, const std::string& aMessage)
{
    const auto& env = g_environment;
    if (!env.sdk || !env.sdk->logger)
        return;

    const char* text = aMessage.c_str();
    switch (aLevel)
    {
    case LogLevel::Trace: env.sdk->logger->TraceF(env.handle, "%s", text); break;
    case LogLevel::Debug: env.sdk->logger->DebugF(env.handle, "%s", text); break;
    case LogLevel::Info: env.sdk->logger->InfoF(env.handle, "%s", text); break;
    case LogLevel::Warn: env.sdk->logger->WarnF(env.handle, "%s", text); break;
    case LogLevel::Error: env.sdk->logger->ErrorF(env.handle, "%s", text); break;
    }
}
} // namespace

Environment& Env()
{
    return g_environment;
}

void Initialize(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk)
{
    auto& env = g_environment;
    env.handle = aHandle;
    env.sdk = aSdk;
    Log::SetSink(&LogToRed4ext);

    env.pluginDir = ModulePath(aHandle).parent_path();
    env.gameExe = ModulePath(nullptr);
    env.gameBuild = FileVersion(env.gameExe);
    std::error_code error;
    env.exeSize = static_cast<uint64_t>(std::filesystem::file_size(env.gameExe, error));
    env.devInstance = ParseDevInstance();
    if (env.devInstance == 0)
        env.devInstance = DetectInstance(env.pluginDir);

    env.settings.Load(env.pluginDir / "coop.ini");
    Log::SetMinLevel(env.settings.GetBool("dev.verbose", false) ? LogLevel::Debug : LogLevel::Info);

    const std::string idFile = env.devInstance > 0 ? "client-id-" + std::to_string(env.devInstance) + ".txt"
                                                   : "client-id.txt";
    env.clientId = LoadOrCreateClientId(env.pluginDir / idFile);

    COOP_LOG_INFO("Cp2077Coop %s loaded: game build %s%s", coop::kVersionString, env.gameBuild.c_str(),
                  env.devInstance > 0 ? (", dev instance " + std::to_string(env.devInstance)).c_str() : "");
}

void Shutdown()
{
    Log::SetSink(nullptr);
    g_environment.sdk = nullptr;
}
} // namespace coop::plugin
