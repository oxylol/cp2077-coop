#include "Game/CustomizationState.h"

#include <RED4ext/Scripting/Utils.hpp>

namespace
{
// IsA on a pointer read from engine memory; false instead of a crash when it isn't an object.
bool SafeIsA(RED4ext::IScriptable* apInstance, const RED4ext::CClass* apClass)
{
    if (!apInstance || !apClass)
        return false;
    const auto address = reinterpret_cast<uintptr_t>(apInstance);
    if (address < 0x10000 || address > 0x00007FFFFFFFFFFF || (address & 0x7) != 0)
        return false;
    __try
    {
        return apInstance->GetType()->IsA(apClass);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// The object pointer at an address (e.g. a handle's instance), or nullptr if it can't be read.
RED4ext::IScriptable* SafeReadPointer(uintptr_t aAddress)
{
    __try
    {
        return *reinterpret_cast<RED4ext::IScriptable**>(aAddress);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

const char* TypeName(const RED4ext::CBaseRTTIType* apType)
{
    return apType ? apType->GetName().ToString() : "void";
}

// The class a handle-typed value points to, or nullptr for any other type.
const RED4ext::CClass* HandleTarget(const RED4ext::CBaseRTTIType* apType)
{
    if (!apType)
        return nullptr;
    const RED4ext::CBaseRTTIType* inner = nullptr;
    if (apType->GetType() == RED4ext::ERTTIType::Handle)
        inner = static_cast<const RED4ext::CRTTIHandleType*>(apType)->GetInnerType();
    else if (apType->GetType() == RED4ext::ERTTIType::WeakHandle)
        inner = static_cast<const RED4ext::CRTTIWeakHandleType*>(apType)->GetInnerType();
    if (!inner || inner->GetType() != RED4ext::ERTTIType::Class)
        return nullptr;
    return static_cast<const RED4ext::CClass*>(inner);
}

void LogFunctions(const RED4ext::CClass* apClass)
{
    for (auto* pClass = apClass; pClass; pClass = pClass->parent)
    {
        spdlog::info("[Customization] functions of {}:", pClass->GetName().ToString());
        for (const auto* pFunc : pClass->funcs)
        {
            std::string params;
            for (const auto* pParam : pFunc->params)
                params += std::string(params.empty() ? "" : ", ") + TypeName(pParam->type);
            spdlog::info("[Customization]   {}({}) -> {}", pFunc->shortName.ToString(), params,
                         pFunc->returnType ? TypeName(pFunc->returnType->type) : "void");
        }
    }
}
} // namespace

Red::Handle<Red::game::ui::CharacterCustomizationState> FindLocalCustomizationState()
{
    using State = Red::game::ui::CharacterCustomizationState;
    static bool s_logged = false;
    static int s_lastSource = -1;

    auto* pSystemClass = Red::GetClass("gameuiCharacterCustomizationSystem");
    const auto* pStateClass = Red::GetClass("gameuiCharacterCustomizationState");
    auto* pSystem = Red::GetGameSystem<Red::game::ui::CharacterCustomizationSystem>();
    if (!pSystemClass || !pStateClass || !pSystem)
    {
        spdlog::warn("[Customization] the character customization system isn't available");
        return {};
    }

    const auto found = [&](Red::Handle<State> aState, int aSource, const std::string& aHow) {
        if (aSource != s_lastSource)
            spdlog::info("[Customization] using the state from {}", aHow);
        s_lastSource = aSource;
        return aState;
    };

    // 1. Where CyberpunkMP found it on patch 2.2.
    auto* pKnown = GetCustomizationState(pSystem);
    if (pKnown && SafeIsA(pKnown->instance, pStateClass))
        return found(*pKnown, 1, "+0x78");

    if (!s_logged)
    {
        s_logged = true;
        spdlog::info("[Customization] nothing at +0x78; looking for the state elsewhere");
        LogFunctions(pSystemClass);
    }

    // 2. A getter without arguments that returns a state.
    for (auto* pClass = pSystemClass; pClass; pClass = pClass->parent)
    {
        for (auto* pFunc : pClass->funcs)
        {
            if (pFunc->params.size != 0 || !pFunc->returnType)
                continue;
            const auto* pTarget = HandleTarget(pFunc->returnType->type);
            if (!pTarget || !pStateClass->IsA(pTarget))
                continue;
            Red::Handle<Red::IScriptable> result;
            Red::StackArgs_t args;
            if (pFunc->returnType->type->GetType() == RED4ext::ERTTIType::WeakHandle)
            {
                Red::WeakHandle<Red::IScriptable> weak;
                if (RED4ext::ExecuteFunction(pSystem, pFunc, &weak, args))
                    result = weak.Lock();
            }
            else
            {
                RED4ext::ExecuteFunction(pSystem, pFunc, &result, args);
            }
            if (SafeIsA(result.instance, pStateClass))
            {
                return found(*reinterpret_cast<Red::Handle<State>*>(&result), 2,
                             std::string(pFunc->shortName.ToString()) + "()");
            }
        }
    }

    // 3. A handle to a state somewhere in the system (the +0x78 field may have moved).
    for (uint32_t offset = 0x40; offset < 0x400; offset += 8)
    {
        const auto address = reinterpret_cast<uintptr_t>(pSystem) + offset;
        if (SafeIsA(SafeReadPointer(address), pStateClass))
        {
            const auto* pHandle = reinterpret_cast<const Red::Handle<State>*>(address);
            return found(*pHandle, 3 + static_cast<int>(offset), fmt::format("the handle at +0x{:X}", offset));
        }
    }

    if (s_lastSource != 0)
        spdlog::warn("[Customization] no customization state found; others will see a default look");
    s_lastSource = 0;
    return {};
}
