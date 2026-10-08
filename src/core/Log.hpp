#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <utility>

namespace coop
{
enum class LogLevel
{
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};

using LogSink = std::function<void(LogLevel, const std::string&)>;

// Process-wide logging used by the portable code. The game plugin routes it to the RED4ext logger,
// the tools route it to stdout. Thread-safe.
class Log
{
public:
    static void SetSink(LogSink aSink);
    static void SetMinLevel(LogLevel aLevel);
    static void Write(LogLevel aLevel, const std::string& aMessage);

    template<typename... Args>
    static void Format(LogLevel aLevel, const char* aFormat, Args&&... aArgs)
    {
        if (!Enabled(aLevel))
            return;

        char buffer[1024];
        if constexpr (sizeof...(Args) == 0)
            std::snprintf(buffer, sizeof(buffer), "%s", aFormat);
        else
            std::snprintf(buffer, sizeof(buffer), aFormat, std::forward<Args>(aArgs)...);
        Write(aLevel, buffer);
    }

    static bool Enabled(LogLevel aLevel);
};

const char* ToString(LogLevel aLevel);
} // namespace coop

#define COOP_LOG_TRACE(...) ::coop::Log::Format(::coop::LogLevel::Trace, __VA_ARGS__)
#define COOP_LOG_DEBUG(...) ::coop::Log::Format(::coop::LogLevel::Debug, __VA_ARGS__)
#define COOP_LOG_INFO(...) ::coop::Log::Format(::coop::LogLevel::Info, __VA_ARGS__)
#define COOP_LOG_WARN(...) ::coop::Log::Format(::coop::LogLevel::Warn, __VA_ARGS__)
#define COOP_LOG_ERROR(...) ::coop::Log::Format(::coop::LogLevel::Error, __VA_ARGS__)
