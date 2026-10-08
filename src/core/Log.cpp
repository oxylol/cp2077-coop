#include "core/Log.hpp"

#include <atomic>
#include <mutex>

namespace coop
{
namespace
{
std::mutex g_sinkMutex;
LogSink g_sink;
std::atomic<int> g_minLevel{static_cast<int>(LogLevel::Info)};
} // namespace

void Log::SetSink(LogSink aSink)
{
    std::scoped_lock lock(g_sinkMutex);
    g_sink = std::move(aSink);
}

void Log::SetMinLevel(LogLevel aLevel)
{
    g_minLevel.store(static_cast<int>(aLevel));
}

bool Log::Enabled(LogLevel aLevel)
{
    return static_cast<int>(aLevel) >= g_minLevel.load();
}

void Log::Write(LogLevel aLevel, const std::string& aMessage)
{
    if (!Enabled(aLevel))
        return;

    std::scoped_lock lock(g_sinkMutex);
    if (g_sink)
    {
        g_sink(aLevel, aMessage);
    }
    else
    {
        std::fprintf(stderr, "[%s] %s\n", ToString(aLevel), aMessage.c_str());
    }
}

const char* ToString(LogLevel aLevel)
{
    switch (aLevel)
    {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
    }
    return "?";
}
} // namespace coop
