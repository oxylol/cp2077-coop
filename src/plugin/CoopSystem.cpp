#include "plugin/CoopSystem.hpp"

#include "core/Log.hpp"
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
}

void CoopSystem::OnTick(Red::FrameInfo&, Red::JobQueue&)
{
    // Main thread: hand the local player to the network thread, apply what arrived from it.
    if (m_runner)
        m_runner->Pump(m_adapter);
}

// ---------------------------------------------------------------------------------------------------------------------
// Helpers

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
