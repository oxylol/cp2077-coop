#pragma once

#include <map>
#include <string>

#include <RED4ext/RED4ext.hpp>
#include <RedLib.hpp>

#include "client/GameAdapter.hpp"

namespace coop::plugin
{
// IGameAdapter on top of the redscript bridge (scripts/Cp2077Coop/CoopBridge.reds).
// The bridge is a scriptable system that registers itself with CoopSystem when a game session is ready.
class RedGameAdapter final : public IGameAdapter
{
public:
    void SetBridge(const Red::Handle<Red::IScriptable>& aBridge);
    [[nodiscard]] bool HasBridge() const;

    bool CaptureLocal(LocalSample& aOut) override;
    LocalAppearance GetLocalAppearance() override;
    void OnRemotePlayerJoined(const RemotePlayerInfo& aInfo) override;
    void OnRemoteAppearance(PeerId aPeer, const LocalAppearance& aAppearance) override;
    void OnRemotePlayerLeft(PeerId aPeer) override;
    void DriveRemotePlayer(PeerId aPeer, const RemotePose& aPose) override;
    void OnStatus(const std::string& aText) override;
    // Passed to the bridge every frame, which applies them to the game's time dilation (spikes S8, S8b).
    void ApplyTimeRates(const TimeRates& aRates) override;
    [[nodiscard]] const TimeRates& LastTimeRates() const { return m_timeRates; }

    // Removes every puppet (leaving a session, or the session ending).
    void RemoveAllPuppets();

    [[nodiscard]] const std::string& LastStatus() const { return m_lastStatus; }

private:
    Red::Handle<Red::IScriptable> Bridge() const;

    Red::WeakHandle<Red::IScriptable> m_bridge;
    std::map<PeerId, std::string> m_names;
    std::map<PeerId, uint8_t> m_bodyGender;
    std::string m_lastStatus;
    TimeRates m_timeRates;
    bool m_warnedCapture = false;
    bool m_localFemale = false; // body gender reported by the bridge in the last capture
};
} // namespace coop::plugin
