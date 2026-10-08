#include "plugin/CoopSystem.hpp"

#include <cctype>

#include <RED4ext/ResourcePath.hpp>
#include <RED4ext/ResourceReference.hpp>
#include <RED4ext/Scripting/Natives/Generated/red/ResourceReferenceScriptToken.hpp>

#include "core/Log.hpp"
#include "plugin/AnimCapture.hpp"
#include "plugin/AnimDiagnostics.hpp"
#include "plugin/BodySetup.hpp"
#include "plugin/Placement.hpp"
#include "plugin/Plugin.hpp"

using namespace coop;

namespace Coop
{
// ---------------------------------------------------------------------------------------------------------------------
// Natives

bool CoopSystem::Host(int32_t aPort, const Red::CString& aPassword)
{
    const auto& env = plugin::Env();

    HostConfig config;
    config.port = aPort > 0 && aPort < 65536 ? static_cast<uint16_t>(aPort)
                                             : static_cast<uint16_t>(env.settings.GetInt("network.port", kDefaultPort));
    config.hostName = DisplayName();
    config.password = aPassword.c_str();
    config.gameBuild = env.gameBuild;
    config.exeSize = env.exeSize;
    config.requireSameBuild = true;
    config.allowSimClients = env.settings.GetBool("dev.allowSimClients", true);

    std::string error;
    if (!Runner().Host(config, MakeClientConfig({}), error))
    {
        m_adapter.OnStatus("could not host: " + error);
        return false;
    }
    m_adapter.OnStatus("hosting on UDP port " + std::to_string(config.port)
                       + (config.password.empty() ? "" : " (password set)"));
    return true;
}

bool CoopSystem::Join(const Red::CString& aAddress, const Red::CString& aPassword)
{
    std::string error;
    if (!Runner().Join(aAddress.c_str(), MakeClientConfig(aPassword.c_str()), error))
    {
        m_adapter.OnStatus("could not join: " + error);
        return false;
    }
    m_adapter.OnStatus(std::string("connecting to ") + aAddress.c_str());
    return true;
}

void CoopSystem::Leave()
{
    ShutdownSession("left");
}

bool CoopSystem::IsActive() const
{
    return m_runner && m_runner->IsActive();
}

bool CoopSystem::IsHost() const
{
    return m_runner && m_runner->IsHost();
}

Red::CString CoopSystem::GetStatus() const
{
    std::string text = m_runner ? m_runner->StatusText() : std::string();
    if (!IsActive())
    {
        if (!m_adapter.LastStatus().empty())
            text = m_adapter.LastStatus();
        return Red::CString(text.empty() ? "not in a session" : text.c_str());
    }

    if (!m_adapter.HasBridge())
    {
        // A script class missing from RTTI means redscript never loaded our scripts; otherwise the bridge
        // only registers once a save is loaded.
        if (!Red::GetClass(Red::CName("CoopBridge")))
            text += "SCRIPTS NOT LOADED: redscript did not compile r6\\scripts\\Cp2077Coop "
                    "(see r6\\logs\\redscript_rCURRENT.log)\n";
        else
            text += "(waiting for the game session: load a save)\n";
        text += "your position is not sent until then, so bots that follow you wait\n";
    }
    return Red::CString(text.c_str());
}

Red::CString CoopSystem::GetVersion() const
{
    return Red::CString(kVersionString);
}

Red::CString CoopSystem::GetSetting(const Red::CString& aKey) const
{
    return Red::CString(plugin::Env().settings.Get(aKey.c_str()).c_str());
}

bool CoopSystem::SetImpairment(const Red::CString& aPreset)
{
    return Runner().SetImpairment(aPreset.c_str());
}

void CoopSystem::SetDisplayName(const Red::CString& aName)
{
    m_displayName = aName.c_str();
}

void CoopSystem::SetBridge(const Red::Handle<Red::IScriptable>& aBridge)
{
    m_adapter.SetBridge(aBridge);
    COOP_LOG_INFO(aBridge ? "script bridge attached" : "script bridge detached");
}

uint32_t CoopSystem::ActivateTimeField(float aScale, float aSeconds)
{
    if (!m_runner)
        return 0;
    return m_runner->ActivateTimeField(msg::TimeFieldKind::Sandevistan, aScale, FromSeconds(aSeconds));
}

void CoopSystem::CancelTimeField(uint32_t aId)
{
    if (m_runner)
        m_runner->CancelTimeField(aId);
}

float CoopSystem::GetWorldRate() const
{
    return IsActive() ? m_adapter.LastTimeRates().worldRate : 1.0f;
}

float CoopSystem::GetLocalRate() const
{
    return IsActive() ? m_adapter.LastTimeRates().localRate : 1.0f;
}

bool CoopSystem::IsActivatingTimeField() const
{
    return IsActive() && m_adapter.LastTimeRates().activating;
}

Red::CString CoopSystem::GetAnimStatus() const
{
    std::string text = plugin::AnimCapture::Get().Status();
    text += std::string("inputs are applied via ")
            + (plugin::GetAnimApplyVia() == plugin::AnimApplyVia::Controller ? "the animation controller" : "events")
            + "\n";
    if (!m_mirror.Expired())
        text += "mirror test: on, " + std::to_string(m_mirrorApplied) + " captured input(s) and "
                + std::to_string(m_mirrorMotionApplied) + " motion input(s) applied\n";
    text += m_adapter.MotionStatus();
    text += plugin::BodySetup::Get().Status();
    if (!m_lastAIError.empty())
        text += "puppet AI switch: " + m_lastAIError + "\n";
    const auto apply = m_adapter.AnimApplyStatus();
    if (!apply.empty())
        text += "applied to puppets:\n" + apply;
    return Red::CString(text.c_str());
}

Red::CString CoopSystem::StartAnimRecording(float aSeconds)
{
    const auto file = plugin::Env().pluginDir / "anim-record.txt";
    plugin::AnimCapture::Get().StartRecording(aSeconds > 0.0f ? aSeconds : 10.0f, file);
    return Red::CString(file.string().c_str());
}

Red::CString CoopSystem::DumpAnimFunctions()
{
    const auto file = plugin::Env().pluginDir / "anim-functions.txt";
    const int count = plugin::AnimCapture::Get().DumpFunctions(file);
    return Red::CString((std::to_string(count) + " functions listed in " + file.string()).c_str());
}

Red::CString CoopSystem::DumpAnimGraphs(const Red::Handle<Red::IScriptable>& aEntity, const Red::CString& aLabel)
{
    std::string label = aLabel.c_str();
    for (auto& c : label)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
            c = '_';
    }
    if (label.empty())
        label = "entity";
    const auto file = plugin::Env().pluginDir / ("anim-graphs-" + label + ".txt");
    return Red::CString(plugin::DumpAnimGraphs(aEntity.instance, label, file).c_str());
}

void CoopSystem::SetAnimApplyVia(const Red::CString& aVia)
{
    const std::string via = aVia.c_str();
    plugin::SetAnimApplyVia(via == "controller" ? plugin::AnimApplyVia::Controller : plugin::AnimApplyVia::Events);
    COOP_LOG_INFO("animation inputs applied via %s", via == "controller" ? "the controller" : "events");
}

Red::CString CoopSystem::GetAnimApplyVia() const
{
    return Red::CString(plugin::GetAnimApplyVia() == plugin::AnimApplyVia::Controller ? "controller" : "events");
}

int32_t CoopSystem::ApplyAnimTest(const Red::Handle<Red::IScriptable>& aEntity, const Red::CString& aName,
                                  float aValue)
{
    std::string name = aName.c_str();
    if (!aEntity || name.empty())
        return 0;
    AnimInput input;
    if (name[0] == '!')
    {
        input.kind = AnimInputKind::Event;
        name.erase(0, 1);
    }
    else
    {
        input.kind = AnimInputKind::Float;
        input.value = AnimValue::FromFloat(aValue);
    }
    input.name = Red::CName(name.c_str()).hash;
    return plugin::ApplyAnimInputs(aEntity.instance, {input});
}

void CoopSystem::OnBodyInitialize(const Red::Handle<Red::IScriptable>& aEvent)
{
    // Codeware's EntityLifecycleEvent for a puppet or mirror body that is being built.
    Red::WeakHandle<Red::IScriptable> entity;
    if (!aEvent || !Red::CallVirtual(aEvent.instance, "GetEntity", entity))
        return;
    if (auto locked = entity.Lock())
        plugin::BodySetup::Get().Prepare(locked.instance);
}

void CoopSystem::SetBodyOptions(bool aAddImpostor, bool aBorrowAnimsets)
{
    plugin::BodyOptions options;
    options.addImpostor = aAddImpostor;
    options.borrowAnimsets = aBorrowAnimsets;
    plugin::BodySetup::Get().SetOptions(options);
}

void CoopSystem::MirrorAnimationsTo(const Red::Handle<Red::IScriptable>& aEntity)
{
    m_mirror = aEntity;
    m_mirrorApplied = 0;
    m_mirrorMotionApplied = 0;
    m_adapter.ResetMirrorMotion();
    plugin::AnimCapture::Get().SetMirrorActive(static_cast<bool>(aEntity));
}

bool CoopSystem::SetPuppetAI(const Red::Handle<Red::IScriptable>& aEntity, bool aEnabled)
{
    if (!aEntity)
        return false;
    // The extra components first, and whether or not the body has an AI: a body without one (e.g. TPP_Player)
    // still needs its movement component off to be placed (round H).
    for (const auto& name : m_switchOff)
        SetEntityComponents(aEntity, Red::CString(name.c_str()), aEnabled);

    Red::Handle<Red::IScriptable> ai;
    if (!Red::CallVirtual(aEntity.instance, "GetAIControllerComponent", ai) || !ai)
    {
        m_lastAIError = "the body has no AI controller (GetAIControllerComponent)";
        COOP_LOG_WARN("puppet AI switch: %s", m_lastAIError.c_str());
        return false;
    }
    bool enabled = aEnabled;
    if (!Red::CallVirtual(ai.instance, "Toggle", enabled))
    {
        m_lastAIError = "IComponent.Toggle could not be called";
        COOP_LOG_WARN("puppet AI switch: %s", m_lastAIError.c_str());
        return false;
    }
    COOP_LOG_INFO("puppet AI switched %s", aEnabled ? "on" : "off");
    m_lastAIError.clear();
    return true;
}

void CoopSystem::SetSwitchOffComponents(const Red::CString& aList)
{
    m_switchOff.clear();
    std::string item;
    for (const char* c = aList.c_str();; ++c)
    {
        if (*c == ',' || *c == ' ' || *c == '\0')
        {
            if (!item.empty())
                m_switchOff.push_back(item);
            item.clear();
            if (*c == '\0')
                break;
        }
        else
        {
            item += *c;
        }
    }
}

Red::CString CoopSystem::GetSwitchOffComponents() const
{
    std::string text;
    for (const auto& name : m_switchOff)
        text += (text.empty() ? "" : ",") + name;
    return Red::CString(text.c_str());
}

bool CoopSystem::PlaceEntity(const Red::Handle<Red::IScriptable>& aEntity, const Red::Vector4& aPosition, float aYaw,
                             int32_t aMethod)
{
    std::string error;
    if (plugin::PlaceEntity(aEntity, aPosition, aYaw, aMethod, error))
        return true;
    if (error != m_placementError)
        COOP_LOG_WARN("placing a body (%s): %s", plugin::PlaceMethodName(aMethod), error.c_str());
    m_placementError = error;
    return false;
}

Red::CString CoopSystem::GetPlacementError() const
{
    return Red::CString(m_placementError.c_str());
}

int32_t CoopSystem::GetPlacementMethod() const
{
    return plugin::ParsePlaceMethod(plugin::Env().settings.Get("puppet.place", "auto"));
}

Red::CString CoopSystem::GetPlacementMethodName(int32_t aMethod) const
{
    return Red::CString(plugin::PlaceMethodName(aMethod));
}

bool CoopSystem::SetSpawnTemplate(const Red::Handle<Red::IScriptable>& aSpec, const Red::CString& aPath)
{
    // Codeware's DynamicEntitySpec.templatePath is a resource async reference (shown as ResRef in scripts), which
    // CET can't build from a string; the path hash is written here.
    if (!aSpec)
        return false;
    auto* prop = aSpec->GetType()->GetProperty(RED4ext::CName("templatePath"));
    if (!prop)
        return false;
    const RED4ext::ResourcePath path(aPath.c_str());
    const auto meta = prop->type->GetType();
    if (meta == RED4ext::rtti::ERTTIType::ResourceAsyncReference)
    {
        prop->GetValuePtr<RED4ext::ResourceAsyncReference<>>(aSpec.instance)->path = path;
    }
    else if (meta == RED4ext::rtti::ERTTIType::Class && prop->type->GetName() == RED4ext::CName("redResourceReferenceScriptToken"))
    {
        prop->GetValuePtr<RED4ext::ResRef>(aSpec.instance)->resource.path = path;
    }
    else
    {
        COOP_LOG_WARN("spawn template: DynamicEntitySpec.templatePath has an unexpected type (%s)",
                      prop->type->GetName().ToString());
        return false;
    }
    COOP_LOG_INFO("spawn template: %s (path hash %016llx)", aPath.c_str(), static_cast<unsigned long long>(path.hash));
    return true;
}

int32_t CoopSystem::SetEntityComponents(const Red::Handle<Red::IScriptable>& aEntity, const Red::CString& aClassName,
                                        bool aEnabled)
{
    std::string error;
    const int count = plugin::SetComponentsEnabled(aEntity.instance, aClassName.c_str(), aEnabled, error);
    if (count < 0)
        COOP_LOG_WARN("switching %s components: %s", aClassName.c_str(), error.c_str());
    else
        COOP_LOG_INFO("switched %d %s component(s) %s", count, aClassName.c_str(), aEnabled ? "on" : "off");
    return count;
}

void CoopSystem::SetMotionInputs(const Red::CString& aSpeed, const Red::CString& aDirection,
                                 const Red::CString& aVertical, const Red::CString& aTurnRate,
                                 const Red::CString& aMoving)
{
    m_adapter.SetMotionInputs({aSpeed.c_str(), aDirection.c_str(), aVertical.c_str(), aTurnRate.c_str(), aMoving.c_str()});
}

Red::CString CoopSystem::GetMotionInputs() const
{
    const auto& names = m_adapter.MotionNameText();
    std::string joined = names[0];
    for (size_t i = 1; i < names.size(); ++i)
        joined += "," + names[i];
    return Red::CString(joined.c_str());
}

void CoopSystem::SetMotionFeature(bool aOn)
{
    m_adapter.SetMotionFeature(aOn);
    COOP_LOG_INFO("motion: walking feature (playerLocomotion) %s", aOn ? "on" : "off");
}

bool CoopSystem::GetMotionFeature() const
{
    return m_adapter.MotionFeature();
}

void CoopSystem::SetTppFeature(bool aOn)
{
    m_adapter.SetTppFeature(aOn);
    COOP_LOG_INFO("motion: third-person feature (TPPRepresentation) %s", aOn ? "on" : "off");
}

bool CoopSystem::GetTppFeature() const
{
    return m_adapter.TppFeature();
}

int32_t CoopSystem::GetBodyOptions() const
{
    const auto options = plugin::BodySetup::Get().Options();
    return (options.addImpostor ? 1 : 0) | (options.borrowAnimsets ? 2 : 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// Engine hooks

void CoopSystem::OnRegisterUpdates(Red::UpdateRegistrar* aRegistrar)
{
    aRegistrar->RegisterUpdate(Red::UpdateTickGroup::FrameBegin, this, "CoopSystem/Tick",
                               {this, &CoopSystem::OnTick});
}

void CoopSystem::OnAfterWorldDetach()
{
    // Loading another save or returning to the main menu ends the session (M0). Keeping the session across
    // save loads is part of the save/join work in M2/M3; fast travel doesn't detach the world.
    if (IsActive())
        ShutdownSession("the game world was unloaded");
    // The mirror body is gone with the world.
    MirrorAnimationsTo({});
    plugin::AnimCapture::Get().SetLocalPlayer(nullptr);
    plugin::BodySetup::Get().ForgetPlayer();
}

void CoopSystem::OnTick(Red::FrameInfo&, Red::JobQueue&)
{
    // The capture points exist once the game's scripts are loaded, which they are by the first game tick.
    if (!m_animStarted)
    {
        m_animStarted = true;
        const auto& settings = plugin::Env().settings;
        const bool capture = settings.GetBool("anim.capture", true);
        m_adapter.SetAnimOptions(capture, settings.GetBool("anim.apply", true));
        SetSwitchOffComponents(Red::CString(settings.Get("puppet.switchOff", "moveComponent").c_str()));
        SetAnimApplyVia(Red::CString(settings.Get("anim.applyVia", "events").c_str()));
        // Graph inputs for the motion values: the names V's graph and the lookalike's both use (round I dumps).
        // Empty (as in coop.ini files copied from 0.5.4 and older) = the default; "none" = not sent.
        const auto motionInput = [&settings](const char* aKey, const char* aDefault) {
            const auto value = settings.Get(aKey);
            return value.empty() ? std::string(aDefault) : value;
        };
        m_adapter.SetMotionInputs({motionInput("anim.speedInput", "speed_horizontal+desired_speed_horizontal"),
                                   motionInput("anim.directionInput", "move_direction"),
                                   motionInput("anim.verticalSpeedInput", "speed_vertical"),
                                   motionInput("anim.turnRateInput", "rotation_speed_yaw"),
                                   motionInput("anim.movingInput", "")});
        m_adapter.SetMotionFeature(settings.GetBool("anim.movementFeature", true));
        m_adapter.SetTppFeature(settings.GetBool("anim.tppFeature", false));
        plugin::BodyOptions body;
        body.addImpostor = settings.GetBool("puppet.addImpostor", false);
        body.borrowAnimsets = settings.GetBool("puppet.borrowAnimsets", false);
        plugin::BodySetup::Get().SetOptions(body);
        if (capture)
            plugin::AnimCapture::Get().Install();
    }

    ++m_ticks;
    if (m_ticks % 600 == 0)
        plugin::ForgetPlacementState();

    // Who the local V is (capture keeps only its inputs; body setup never touches it), and its animation graph
    // (for bodies that borrow it).
    if (m_ticks % 10 == 1)
    {
        auto* player = LocalPlayer();
        plugin::AnimCapture::Get().SetLocalPlayer(player);
        plugin::BodySetup::Get().RememberPlayer(player);
    }
    // Codeware's Entity/Initialize for puppet and mirror bodies (registered once).
    if (m_ticks % 60 == 1 && !plugin::BodySetup::Get().IsRegistered())
    {
        // A strong handle from the system's own reference count (the engine owns it). Converted to
        // Handle<IScriptable> by reference, not through WeakHandle's conversion operator.
        const Red::Handle<CoopSystem> self = Red::AsWeakHandle(this).Lock();
        plugin::BodySetup::Get().EnsureRegistered(self);
    }

    // Main thread: hand the local player to the network thread, apply what arrived from it.
    if (m_runner)
        m_runner->Pump(m_adapter);

    // One-game test: your own animation inputs applied to a lookalike next to you.
    if (const auto mirror = m_mirror.Lock())
    {
        std::vector<AnimInput> inputs;
        plugin::AnimCapture::Get().DrainForMirror(inputs);
        if (!inputs.empty())
            m_mirrorApplied += static_cast<uint64_t>(plugin::ApplyAnimInputs(mirror.instance, inputs));
        m_mirrorMotionApplied += static_cast<uint64_t>(m_adapter.DriveMirrorMotion(mirror.instance));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Helpers

RED4ext::IScriptable* CoopSystem::LocalPlayer() const
{
    Red::ScriptGameInstance game;
    Red::Handle<Red::IScriptable> system;
    if (!Red::CallStatic("ScriptGameInstance", "GetPlayerSystem", system, game) || !system)
        return nullptr;
    Red::Handle<Red::IScriptable> player;
    if (!Red::CallVirtual(system.instance, "GetLocalPlayerMainGameObject", player) || !player)
    {
        if (!Red::CallVirtual(system.instance, "GetLocalPlayerControlledGameObject", player) || !player)
            return nullptr;
    }
    return player.instance; // compared by address only; the player system keeps it alive
}

SessionRunner& CoopSystem::Runner()
{
    if (!m_runner)
        m_runner = std::make_unique<SessionRunner>(m_clock);
    return *m_runner;
}

std::string CoopSystem::DisplayName() const
{
    if (!m_displayName.empty())
        return m_displayName;
    const auto& env = plugin::Env();
    std::string name = env.settings.Get("player.name", "V");
    if (env.devInstance > 0)
        name += " " + std::to_string(env.devInstance);
    return name;
}

ClientConfig CoopSystem::MakeClientConfig(const std::string& aPassword) const
{
    const auto& env = plugin::Env();
    ClientConfig config;
    config.displayName = DisplayName();
    config.password = aPassword;
    config.gameBuild = env.gameBuild;
    config.exeSize = env.exeSize;
    config.clientId = env.clientId;
    return config;
}

void CoopSystem::ShutdownSession(const std::string& aReason)
{
    if (m_runner)
    {
        m_runner->Leave(aReason);
        m_runner->Pump(m_adapter); // deliver the "left" events now
    }
    m_adapter.RemoveAllPuppets();
}
} // namespace Coop
