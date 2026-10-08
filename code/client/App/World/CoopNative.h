#pragma once

// Engine calls for the co-op plugin (code/assets/redscript/Plugins/Coop.reds, code/server/scripting/CoopSystem).
//
// Each one looks the game function up by name at runtime and fills in any optional parameters, so a function that
// was renamed or changed on a newer patch logs a warning (once) and returns a "didn't work" value, instead of
// stopping every script from compiling as a wrong redscript declaration would.
struct CoopNative : RED4ext::IScriptable
{
    RTTI_IMPL_TYPEINFO(CoopNative);
    RTTI_IMPL_ALLOCATOR();

    // Game time in seconds since the start of the game (day * 86400 + time of day), or -1.
    static int32_t GetGameTime();
    // Moves the game time forward to the given time; false if the game wouldn't.
    static bool SetGameTime(int32_t aSeconds);
    // The current weather state's name (e.g. 24h_weather_rain), or None.
    static Red::CName GetWeather();
    // Blends to this weather over a few seconds; false if the game wouldn't.
    static bool SetWeather(Red::CName aWeather);
    // Gives the weather back to the game (e.g. when this player becomes the host or leaves).
    static bool ResetWeather();
    // The tracked quest entry as a path of journal ids (quest/phase/objective), or "".
    static Red::CString GetTrackedQuest();
};

RTTI_DEFINE_CLASS(CoopNative, {
    RTTI_ALIAS("CyberpunkMP.Plugins.CoopNative");
    RTTI_METHOD(GetGameTime);
    RTTI_METHOD(SetGameTime);
    RTTI_METHOD(GetWeather);
    RTTI_METHOD(SetWeather);
    RTTI_METHOD(ResetWeather);
    RTTI_METHOD(GetTrackedQuest);
});
