#include "CoopNative.h"

#include <memory>
#include <mutex>
#include <set>

#include <RED4ext/Scripting/Utils.hpp>

namespace
{
// Logs a problem once per message, so a missing function doesn't fill the log every few seconds.
void WarnOnce(const std::string& aMessage)
{
    static std::mutex s_mutex;
    static std::set<std::string> s_seen;
    std::scoped_lock _(s_mutex);
    if (s_seen.insert(aMessage).second)
        spdlog::warn("[Coop] {}", aMessage);
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
    const bool ok = RED4ext::ExecuteFunction(apInstance, pFunc, apOut, args);
    for (auto& entry : defaults)
        entry.type->Destruct(entry.value);
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
} // namespace

int32_t CoopNative::GetGameTime()
{
    const auto time = System("GetTimeSystem");
    float stamp = 0.0f;
    if (time && Call(time.instance, "GetGameTimeStamp", &stamp, {}))
        return static_cast<int32_t>(stamp);
    return -1;
}

bool CoopNative::SetGameTime(int32_t aSeconds)
{
    const auto time = System("GetTimeSystem");
    if (!time || aSeconds < 0)
        return false;
    // Only ever forward: going back in time confuses quest timers.
    const auto now = GetGameTime();
    if (now >= 0 && aSeconds <= now)
        return false;
    spdlog::info("[Coop] game time {} -> {}", now, aSeconds);
    return Call(time.instance, "SetGameTimeBySeconds", nullptr, {{"Int32", &aSeconds}});
}

Red::CName CoopNative::GetWeather()
{
    const auto weather = System("GetWeatherSystem");
    Red::Handle<Red::IScriptable> state;
    Red::CName name;
    if (weather && Call(weather.instance, "GetWeatherState", &state, {}) && state)
        ReadProperty(state.instance, "name", name);
    return name;
}

bool CoopNative::SetWeather(Red::CName aWeather)
{
    const auto weather = System("GetWeatherSystem");
    if (!weather || !aWeather)
        return false;
    float blendTime = 10.0f;
    uint32_t priority = 9; // as the weather mods and the game's console examples use
    spdlog::info("[Coop] weather -> {}", aWeather.ToString());
    return Call(weather.instance, "SetWeather", nullptr,
                {{"CName", &aWeather}, {"Float", &blendTime}, {"Uint32", &priority}});
}

bool CoopNative::ResetWeather()
{
    const auto weather = System("GetWeatherSystem");
    return weather && Call(weather.instance, "ResetWeather", nullptr, {});
}

Red::CString CoopNative::GetTrackedQuest()
{
    const auto journal = System("GetJournalManager");
    if (!journal)
        return {};
    Red::WeakHandle<Red::IScriptable> entry;
    if (!Call(journal.instance, "GetTrackedEntry", &entry, {}))
        return {};
    std::string path;
    for (int depth = 0; depth < 8; ++depth)
    {
        auto locked = entry.Lock();
        if (!locked)
            break;
        Red::CString id;
        if (!Call(locked.instance, "GetId", &id, {}))
            break;
        path = std::string(id.c_str()) + (path.empty() ? "" : "/") + path;
        Red::WeakHandle<Red::IScriptable> parent;
        if (!Call(journal.instance, "GetParentEntry", &parent, {{nullptr, &entry}}))
            break;
        entry = parent;
    }
    return Red::CString(path.c_str());
}
