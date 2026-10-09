#include "SpdlogProvider.hpp"
#include "Core/Facades/Runtime.hpp"
#include "Core/Stl.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include "CrashLog.h"

namespace
{
// 1 for the first game running the mod, 2 for a second one on the same PC (testing), and so on, so each writes a log
// of its own instead of truncating the first one's. Held until the game exits.
int ClaimInstanceNumber()
{
    for (int number = 1; number < 10; ++number)
    {
        const auto name = L"Local\\CyberpunkCoop.Instance." + std::to_wstring(number);
        const HANDLE mutex = CreateMutexW(nullptr, FALSE, name.c_str());
        if (mutex && GetLastError() != ERROR_ALREADY_EXISTS)
            return number;
        if (mutex)
            CloseHandle(mutex);
    }
    return 10;
}
} // namespace

void Support::SpdlogProvider::OnInitialize()
{
    if (m_logPath.empty())
    {
        // CyberpunkCoop.log, CyberpunkCoop-2.log for a second game on this PC, ...
        const auto number = ClaimInstanceNumber();
        auto path = Core::Runtime::GetModulePath();
        if (number > 1)
            path.replace_filename(path.stem().wstring() + L"-" + std::to_wstring(number) + path.extension().wstring());
        m_logPath = path.replace_extension(L".log");
    }

    auto sink = Core::MakeShared<spdlog::sinks::basic_file_sink_mt>(m_logPath.wstring(), true);
    auto logger = Core::MakeShared<spdlog::logger>("", spdlog::sinks_init_list{sink});
    logger->flush_on(spdlog::level::trace);

    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::trace);

    CrashLog::Install(m_logPath);

    SetDefault(*this);
}

void Support::SpdlogProvider::LogInfo(const std::string_view& aMessage)
{
    spdlog::default_logger_raw()->info(aMessage);
}

void Support::SpdlogProvider::LogWarning(const std::string_view& aMessage)
{
    spdlog::default_logger_raw()->warn(aMessage);
}

void Support::SpdlogProvider::LogError(const std::string_view& aMessage)
{
    spdlog::default_logger_raw()->error(aMessage);
}

void Support::SpdlogProvider::LogDebug(const std::string_view& aMessage)
{
    spdlog::default_logger_raw()->debug(aMessage);
}

void Support::SpdlogProvider::LogFlush()
{
    spdlog::default_logger_raw()->flush();
}

void Support::SpdlogProvider::OnShutdown()
{
    CrashLog::Uninstall();
}
