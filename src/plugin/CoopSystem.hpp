#pragma once

#include <memory>
#include <string>

#include <RED4ext/RED4ext.hpp>
#include <RedLib.hpp>

#include "client/SessionRunner.hpp"
#include "core/Clock.hpp"
#include "plugin/RedGameAdapter.hpp"

namespace Coop
{
// Native game system that owns the co-op session in the game process (docs/01-architecture.md §3.3).
// Networking runs on SessionRunner's own thread; this system pumps it once per frame on the main thread,
// which is where every game call happens. Reachable from scripts as GameInstance.GetCoopSystem().
class CoopSystem : public Red::IGameSystem
{
public:
    // --- natives (scripts/Cp2077Coop/CoopNative.reds) ---
    bool Host(int32_t aPort, const Red::CString& aPassword);
    bool Join(const Red::CString& aAddress, const Red::CString& aPassword);
    void Leave();
    [[nodiscard]] bool IsActive() const;
    [[nodiscard]] bool IsHost() const;
    Red::CString GetStatus() const;
    Red::CString GetSetting(const Red::CString& aKey) const;
    bool SetImpairment(const Red::CString& aPreset);
    void SetDisplayName(const Red::CString& aName);
    void SetBridge(const Red::Handle<Red::IScriptable>& aBridge);
    // Time fields: the local V's Sandevistan (scale 0.05-1, seconds up to 30), and this frame's rates.
    uint32_t ActivateTimeField(float aScale, float aSeconds);
    void CancelTimeField(uint32_t aId);
    [[nodiscard]] float GetWorldRate() const;
    [[nodiscard]] float GetLocalRate() const;
    [[nodiscard]] bool IsActivatingTimeField() const;

    // --- engine ---
    void OnRegisterUpdates(Red::UpdateRegistrar* aRegistrar) override;
    void OnAfterWorldDetach() override;

private:
    void OnTick(Red::FrameInfo& aFrame, Red::JobQueue& aJobQueue);
    coop::SessionRunner& Runner();
    std::string DisplayName() const;
    coop::ClientConfig MakeClientConfig(const std::string& aPassword) const;
    void ShutdownSession(const std::string& aReason);

    coop::SteadyClock m_clock; // declared before the runner, which keeps a reference to it
    coop::plugin::RedGameAdapter m_adapter;
    std::unique_ptr<coop::SessionRunner> m_runner;

    std::string m_displayName;

    RTTI_IMPL_TYPEINFO(Coop::CoopSystem);
    RTTI_IMPL_ALLOCATOR();
};
} // namespace Coop

RTTI_DEFINE_CLASS(Coop::CoopSystem, {
    RTTI_METHOD(Host);
    RTTI_METHOD(Join);
    RTTI_METHOD(Leave);
    RTTI_METHOD(IsActive);
    RTTI_METHOD(IsHost);
    RTTI_METHOD(GetStatus);
    RTTI_METHOD(GetSetting);
    RTTI_METHOD(SetImpairment);
    RTTI_METHOD(SetDisplayName);
    RTTI_METHOD(SetBridge);
    RTTI_METHOD(ActivateTimeField);
    RTTI_METHOD(CancelTimeField);
    RTTI_METHOD(GetWorldRate);
    RTTI_METHOD(GetLocalRate);
    RTTI_METHOD(IsActivatingTimeField);
});
