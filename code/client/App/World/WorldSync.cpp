#include "WorldSync.h"

#include <memory>
#include <mutex>
#include <set>

#include <RED4ext/Scripting/Utils.hpp>

#include "App/Network/NetworkService.h"

namespace
{
// Logs a problem once per message, so a missing function doesn't fill the log every few seconds.
void WarnOnce(const std::string& aMessage)
{
    static std::mutex s_mutex;
    static std::set<std::string> s_seen;
    std::scoped_lock _(s_mutex);
    if (s_seen.insert(aMessage).second)
        spdlog::warn("[WorldSync] {}", aMessage);
}

struct Arg
{
    const char* type; // expected parameter type name, or nullptr for any handle
    void* value;
};

bool IsHandle(const RED4ext::CBaseRTTIType* apType)
{
    const auto kind = apType->GetType();
    return kind == RED4ext::ERTTIType::Handle || kind == RED4ext::ERTTIType::WeakHandle;
}

// Calls aName on aInstance with aArgs as its first arguments and every further (optional) parameter at its type's
// default. False, with one warning, if the function is missing or takes other types.
bool Call(RED4ext::IScriptable* apInstance, const char* aName, void* apOut, std::initializer_list<Arg> aArgs)
{
    if (!apInstance)
        return false;
    auto* pFunc = apInstance->GetType()->GetFunction(RED4ext::CName(aName));
    const std::string where = std::string(apInstance->GetType()->GetName().ToString()) + "." + aName;
    if (!pFunc)
    {
        WarnOnce(where + " doesn't exist in this game version");
        return false;
    }
    if (aArgs.size() > pFunc->params.size)
    {
        WarnOnce(where + " takes fewer parameters than expected");
        return false;
    }

    struct Default
    {
        RED4ext::CBaseRTTIType* type;
        std::unique_ptr<uint8_t[]> storage;
        void* value;
    };
    std::vector<Default> defaults;
    Red::StackArgs_t args;
    uint32_t index = 0;
    for (const auto& arg : aArgs)
    {
        auto* pType = pFunc->params[index]->type;
        const bool fits = arg.type ? pType->GetName() == RED4ext::CName(arg.type) : IsHandle(pType);
        if (!fits)
        {
            WarnOnce(where + ": parameter " + std::to_string(index + 1) + " is " + pType->GetName().ToString());
            return false;
        }
        args.emplace_back(pType, arg.value);
        ++index;
    }
    for (; index < pFunc->params.size; ++index)
    {
        auto* pType = pFunc->params[index]->type;
        Default entry{pType, std::make_unique<uint8_t[]>(pType->GetSize() + 16), nullptr};
        entry.value = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(entry.storage.get()) + 15) & ~uintptr_t{15});
        pType->Construct(entry.value);
        args.emplace_back(pType, entry.value);
        defaults.push_back(std::move(entry));
    }
    // A function that returns something isn't called at all without somewhere to put the result (ExecuteFunction
    // returns false; SetWeather never ran): a throwaway one when the caller doesn't want it.
    void* pOut = apOut;
    std::optional<Default> result;
    if (!pOut && pFunc->returnType)
    {
        auto* pType = pFunc->returnType->type;
        result.emplace(Default{pType, std::make_unique<uint8_t[]>(pType->GetSize() + 16), nullptr});
        result->value = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(result->storage.get()) + 15) & ~uintptr_t{15});
        pType->Construct(result->value);
        pOut = result->value;
    }

    const bool ok = RED4ext::ExecuteFunction(apInstance, pFunc, pOut, args);
    for (auto& entry : defaults)
        entry.type->Destruct(entry.value);
    if (result)
        result->type->Destruct(result->value);
    if (!ok)
        WarnOnce(where + " failed");
    return ok;
}

// A game system through its GameInstance getter (e.g. GetTimeSystem).
Red::Handle<Red::IScriptable> System(const char* aGetter)
{
    Red::ScriptGameInstance game;
    Red::Handle<Red::IScriptable> system;
    if (!Red::CallStatic("ScriptGameInstance", aGetter, system, game) || !system)
        WarnOnce(std::string("GameInstance.") + aGetter + " returned nothing");
    return system;
}

template<typename T>
bool ReadProperty(RED4ext::IScriptable* apObject, const char* aName, T& aOut)
{
    if (!apObject)
        return false;
    const auto* pProp = apObject->GetType()->GetProperty(RED4ext::CName(aName));
    if (!pProp || pProp->type->GetSize() != sizeof(T))
        return false;
    aOut = *pProp->GetValuePtr<T>(apObject);
    return true;
}

// The engine calls. Each looks the game function up by name at runtime and fills in any optional parameters, so a
// function renamed or changed on a newer patch logs a warning (once) and does nothing, instead of crashing.

// Game time in seconds since the start of the game (day * 86400 + time of day), or -1.
int32_t GetGameTime()
{
    const auto time = System("GetTimeSystem");
    float stamp = 0.0f;
    if (time && Call(time.instance, "GetGameTimeStamp", &stamp, {}))
        return static_cast<int32_t>(stamp);
    return -1;
}

bool SetGameTime(int32_t aSeconds)
{
    const auto time = System("GetTimeSystem");
    if (!time || aSeconds < 0)
        return false;
    // Only ever forward: going back in time confuses quest timers.
    const auto now = GetGameTime();
    if (now >= 0 && aSeconds <= now)
        return false;
    spdlog::info("[WorldSync] game time {} -> {}", now, aSeconds);
    return Call(time.instance, "SetGameTimeBySeconds", nullptr, {{"Int32", &aSeconds}});
}

Red::CName GetWeather()
{
    const auto weather = System("GetWeatherSystem");
    Red::Handle<Red::IScriptable> state;
    Red::CName name;
    if (weather && Call(weather.instance, "GetWeatherState", &state, {}) && state)
        ReadProperty(state.instance, "name", name);
    return name;
}

bool SetWeather(Red::CName aWeather)
{
    const auto weather = System("GetWeatherSystem");
    if (!weather || !aWeather)
        return false;
    float blendTime = 10.0f;
    uint32_t priority = 9; // as the weather mods and the game's console examples use
    spdlog::info("[WorldSync] weather -> {}", aWeather.ToString());
    return Call(weather.instance, "SetWeather", nullptr,
                {{"CName", &aWeather}, {"Float", &blendTime}, {"Uint32", &priority}});
}

bool ResetWeather()
{
    const auto weather = System("GetWeatherSystem");
    return weather && Call(weather.instance, "ResetWeather", nullptr, {});
}
} // namespace

void WorldSync::Report(NetworkService& aService)
{
    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextReport)
        return;
    m_nextReport = now + kReportInterval;

    const auto gameTime = GetGameTime();
    if (gameTime < 0)
        return;

    client::ReportWorldState report;
    report.set_game_time(static_cast<uint32_t>(gameTime));
    report.set_weather(GetWeather().hash);
    aService.Send(report);
}

void WorldSync::Apply(uint32_t aGameTime, uint64_t aWeather)
{
    const auto mine = GetGameTime();
    if (mine >= 0)
    {
        // How far the host's time of day is ahead of ours. Time only moves forward (going back confuses quest timers),
        // small drifts are left alone, and a guest just a little ahead doesn't skip a whole day.
        constexpr int64_t kDay = 86400;
        auto ahead = static_cast<int64_t>(aGameTime % kDay) - (mine % kDay);
        if (ahead < 0)
            ahead += kDay;
        if (ahead > 120 && ahead < kDay - 600)
            SetGameTime(static_cast<int32_t>(mine + ahead));
    }

    const Red::CName weather(aWeather);
    if (aWeather != 0 && weather != Red::CName("None") && weather != m_weather && SetWeather(weather))
        m_weather = weather;
}

void WorldSync::Reset()
{
    // The weather is ours again.
    if (m_weather)
    {
        ResetWeather();
        m_weather = Red::CName();
    }
    m_nextReport = {};
}
