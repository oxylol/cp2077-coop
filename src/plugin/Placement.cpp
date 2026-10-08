#include "plugin/Placement.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

#include <RED4ext/Scripting/Natives/Generated/WorldTransform.hpp>
#include <RED4ext/Scripting/Natives/ScriptGameInstance.hpp>
#include <RED4ext/Scripting/Natives/entEntity.hpp>
#include <RED4ext/Scripting/Natives/entIComponent.hpp>

#include "core/Math.hpp"

namespace coop::plugin
{
namespace
{
// The last AI teleport command per body, cancelled before the next one so they don't pile up.
std::map<RED4ext::IScriptable*, std::pair<Red::WeakHandle<Red::IScriptable>, Red::Handle<Red::IScriptable>>> g_aiCommands;

Red::Quaternion YawToQuaternion(float aYaw)
{
    const auto q = Quat::FromYawDegrees(aYaw);
    Red::Quaternion result;
    result.i = q.x;
    result.j = q.y;
    result.k = q.z;
    result.r = q.w;
    return result;
}

bool Teleport(const Red::Handle<Red::IScriptable>& aEntity, const Red::Vector4& aPosition, float aYaw,
              std::string& aError)
{
    Red::ScriptGameInstance game;
    Red::Handle<Red::IScriptable> facility;
    if (!Red::CallStatic("ScriptGameInstance", "GetTeleportationFacility", facility, game) || !facility)
    {
        aError = "GetTeleportationFacility could not be called";
        return false;
    }
    Red::Handle<Red::IScriptable> object = aEntity;
    Red::EulerAngles rotation{};
    rotation.Yaw = aYaw;
    auto position = aPosition;
    if (!Red::CallVirtual(facility.instance, "Teleport", object, position, rotation))
    {
        aError = "TeleportationFacility.Teleport could not be called";
        return false;
    }
    return true;
}

bool SetTransform(const Red::Handle<Red::IScriptable>& aEntity, const Red::Vector4& aPosition, float aYaw,
                  std::string& aError)
{
    Red::WorldTransform transform{};
    transform.Position = Red::WorldPosition(aPosition);
    transform.Orientation = YawToQuaternion(aYaw);
    if (!Red::CallVirtual(aEntity.instance, "SetWorldTransform", transform))
    {
        aError = "Entity.SetWorldTransform could not be called (Codeware too old?)";
        return false;
    }
    return true;
}

bool AITeleport(const Red::Handle<Red::IScriptable>& aEntity, const Red::Vector4& aPosition, float aYaw,
                std::string& aError)
{
    Red::Handle<Red::IScriptable> ai;
    if (!Red::CallVirtual(aEntity.instance, "GetAIControllerComponent", ai) || !ai)
    {
        aError = "the body has no AI controller";
        return false;
    }
    auto* cls = Red::GetClass("AITeleportCommand");
    if (!cls)
    {
        aError = "AITeleportCommand not found";
        return false;
    }
    auto command = Red::MakeScriptedHandle<Red::IScriptable>(cls);
    if (!command)
    {
        aError = "AITeleportCommand could not be created";
        return false;
    }
    auto* position = cls->GetProperty("position");
    auto* rotation = cls->GetProperty("rotation");
    auto* navTest = cls->GetProperty("doNavTest");
    if (!position || !rotation || !navTest)
    {
        aError = "AITeleportCommand has other fields than expected";
        return false;
    }
    position->SetValue<Red::Vector4>(command.instance, aPosition);
    rotation->SetValue<float>(command.instance, aYaw);
    navTest->SetValue<bool>(command.instance, false);

    // Cancel this body's previous command first.
    auto previous = g_aiCommands.find(aEntity.instance);
    if (previous != g_aiCommands.end())
    {
        if (!previous->second.first.Expired() && previous->second.second)
        {
            bool cancelled = false;
            Red::CallVirtual(ai.instance, "CancelCommand", cancelled, previous->second.second);
        }
        g_aiCommands.erase(previous);
    }

    bool sent = false;
    if (!Red::CallVirtual(ai.instance, "SendCommand", sent, command) || !sent)
    {
        aError = "AIHumanComponent.SendCommand refused the teleport command";
        return false;
    }
    g_aiCommands[aEntity.instance] = {Red::WeakHandle<Red::IScriptable>(aEntity), command};
    return true;
}
} // namespace

const char* PlaceMethodName(int32_t aMethod)
{
    switch (static_cast<PlaceMethod>(aMethod))
    {
    case PlaceMethod::Auto: return "auto";
    case PlaceMethod::Teleport: return "teleport";
    case PlaceMethod::Transform: return "transform";
    case PlaceMethod::AITeleport: return "aiteleport";
    default: return "?";
    }
}

int32_t ParsePlaceMethod(const std::string& aText)
{
    std::string text = aText;
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (int32_t method = 1; method < static_cast<int32_t>(PlaceMethod::Count); ++method)
    {
        if (text == PlaceMethodName(method))
            return method;
    }
    return static_cast<int32_t>(PlaceMethod::Auto);
}

bool PlaceMethodNeedsAI(int32_t aMethod)
{
    return aMethod == static_cast<int32_t>(PlaceMethod::AITeleport);
}

bool PlaceEntity(const Red::Handle<Red::IScriptable>& aEntity, const Red::Vector4& aPosition, float aYaw,
                 int32_t aMethod, std::string& aError)
{
    if (!aEntity)
    {
        aError = "no body";
        return false;
    }
    Red::Vector4 position = aPosition;
    position.W = 1.0f;
    switch (static_cast<PlaceMethod>(aMethod))
    {
    case PlaceMethod::Teleport: return Teleport(aEntity, position, aYaw, aError);
    case PlaceMethod::Transform: return SetTransform(aEntity, position, aYaw, aError);
    case PlaceMethod::AITeleport: return AITeleport(aEntity, position, aYaw, aError);
    default: aError = "unknown placement method"; return false;
    }
}

int SetComponentsEnabled(Red::IScriptable* aEntity, const std::string& aClassName, bool aEnabled, std::string& aError)
{
    static auto* entityClass = Red::GetClass("entEntity");
    if (!aEntity || !entityClass || !aEntity->GetType()->IsA(entityClass))
    {
        aError = "not an entity";
        return -1;
    }
    auto* wanted = Red::GetClass(Red::CName(aClassName.c_str()));
    if (!wanted)
    {
        aError = "no component class named " + aClassName;
        return -1;
    }
    auto* entity = reinterpret_cast<RED4ext::ent::Entity*>(aEntity);
    int count = 0;
    for (auto& component : entity->components)
    {
        if (!component || !component->GetType()->IsA(wanted))
            continue;
        bool on = aEnabled;
        if (Red::CallVirtual(component.instance, "Toggle", on))
            ++count;
    }
    return count;
}

void ForgetPlacementState()
{
    for (auto it = g_aiCommands.begin(); it != g_aiCommands.end();)
    {
        if (it->second.first.Expired())
            it = g_aiCommands.erase(it);
        else
            ++it;
    }
}
} // namespace coop::plugin
