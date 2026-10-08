#pragma once

#include <memory>
#include <string>
#include <vector>

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
    // The plugin's version (core/Version.hpp), so the dev panel can tell an old build from the current one.
    Red::CString GetVersion() const;
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
    // Animation capture and direct-drive puppets (docs/07-testing-guide.md).
    Red::CString GetAnimStatus() const;
    Red::CString StartAnimRecording(float aSeconds);
    Red::CString DumpAnimFunctions();
    // Writes what the animation graphs of this entity accept to red4ext/plugins/Cp2077Coop/anim-graphs-<label>.txt.
    Red::CString DumpAnimGraphs(const Red::Handle<Red::IScriptable>& aEntity, const Red::CString& aLabel);
    // "events" (default) or "controller": how inputs are applied to puppets and the mirror.
    void SetAnimApplyVia(const Red::CString& aVia);
    Red::CString GetAnimApplyVia() const;
    // Applies one test input to an entity: "name" sets a float input, "!name" pushes an event. Returns 1 if applied.
    int32_t ApplyAnimTest(const Red::Handle<Red::IScriptable>& aEntity, const Red::CString& aName, float aValue);
    // Applies your own V's animation inputs to this entity every frame (one-game test); null stops.
    void MirrorAnimationsTo(const Red::Handle<Red::IScriptable>& aEntity);
    // Switches a puppet's AI controller on or off (direct drive), together with the component classes listed in
    // coop.ini [puppet] switchOff. Done here rather than in the bridge so that a game version without these
    // functions logs an error instead of stopping every script from compiling.
    bool SetPuppetAI(const Red::Handle<Red::IScriptable>& aEntity, bool aEnabled);
    // Comma-separated component classes switched along with the AI (overrides coop.ini until the game restarts).
    void SetSwitchOffComponents(const Red::CString& aList);
    Red::CString GetSwitchOffComponents() const;
    // Puts a body at a position facing yaw (degrees) with one of the placement methods (src/plugin/Placement.hpp:
    // 1 teleport, 2 transform, 3 AI teleport). False if the call failed; GetPlacementError says why.
    bool PlaceEntity(const Red::Handle<Red::IScriptable>& aEntity, const Red::Vector4& aPosition, float aYaw,
                     int32_t aMethod);
    Red::CString GetPlacementError() const;
    // coop.ini [puppet] place: 0 auto, or a fixed method.
    int32_t GetPlacementMethod() const;
    Red::CString GetPlacementMethodName(int32_t aMethod) const;
    // Points a Codeware DynamicEntitySpec at an entity template by its path (instead of a TweakDB record). Not for
    // characters: a PlayerPuppet or NPC spawned from a bare template has no Character record and crashes the game
    // (round O); they get records (tweaks/Cp2077Coop/bodies.tweak).
    bool SetSpawnTemplate(const Red::Handle<Red::IScriptable>& aSpec, const Red::CString& aPath);
    // Switches every component of this class (e.g. "moveComponent") on or off; returns how many, -1 on error.
    int32_t SetEntityComponents(const Red::Handle<Red::IScriptable>& aEntity, const Red::CString& aClassName,
                                bool aEnabled);
    // Graph input names for the motion values (empty = not sent); overrides coop.ini [anim] until the game restarts.
    void SetMotionInputs(const Red::CString& aSpeed, const Red::CString& aDirection, const Red::CString& aVertical,
                         const Red::CString& aTurnRate, const Red::CString& aMoving);
    // The five names in use, comma-separated in that order ("" = not sent, "-name" = negated).
    Red::CString GetMotionInputs() const;
    // Whether the motion values also go out as V's graph's walking feature (playerLocomotion).
    void SetMotionFeature(bool aOn);
    bool GetMotionFeature() const;
    // Whether placed bodies are also told to animate as third person (TPPRepresentation feature).
    void SetTppFeature(bool aOn);
    bool GetTppFeature() const;
    // Codeware calls this (Entity/Initialize) for puppet and mirror bodies being built (src/plugin/BodySetup.hpp).
    void OnBodyInitialize(const Red::Handle<Red::IScriptable>& aEvent);
    // Bodies spawned from then on: an impostor added to player bodies without one; V's gameplay animation sets
    // lent to NPC bodies (src/plugin/BodySetup.hpp).
    void SetBodyOptions(bool aAddImpostor, bool aBorrowAnimsets);
    // The current body options: 1 = add impostor, 2 = borrow animation sets.
    int32_t GetBodyOptions() const;
    // Your V's look on a body (CyberpunkMP's method, src/plugin/Looks.hpp): third-person flag, your items, your
    // character customization. Returns what was done.
    Red::CString ApplyMyLook(const Red::Handle<Red::IScriptable>& aEntity);
    // Which steps looks use, for the mirror and for puppets from then on.
    void SetLookOptions(bool aThirdPerson, bool aItems, bool aCustomization);
    // 1 = third person, 2 = items, 4 = customization.
    int32_t GetLookOptions() const;
    // Whether puppets get their own player's look (coop.ini [look] apply).
    void SetPuppetLooks(bool aOn);
    bool GetPuppetLooks() const;

    // --- engine ---
    void OnRegisterUpdates(Red::UpdateRegistrar* aRegistrar) override;
    void OnAfterWorldDetach() override;

private:
    void OnTick(Red::FrameInfo& aFrame, Red::JobQueue& aJobQueue);
    coop::SessionRunner& Runner();
    std::string DisplayName() const;
    coop::ClientConfig MakeClientConfig(const std::string& aPassword) const;
    void ShutdownSession(const std::string& aReason);
    // The local V from the player system (nullptr while there is none).
    RED4ext::IScriptable* LocalPlayer() const;
    // Whether the local V has a female body (the bridge's last answer).
    bool LocalFemale() const;

    coop::SteadyClock m_clock; // declared before the runner, which keeps a reference to it
    coop::plugin::RedGameAdapter m_adapter;
    std::unique_ptr<coop::SessionRunner> m_runner;

    std::string m_displayName;
    Red::WeakHandle<Red::IScriptable> m_mirror;
    uint64_t m_mirrorApplied = 0;
    uint64_t m_mirrorMotionApplied = 0;
    std::string m_lastAIError;
    std::string m_placementError;
    std::vector<std::string> m_switchOff;
    uint64_t m_ticks = 0;
    bool m_animStarted = false;

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
    RTTI_METHOD(GetVersion);
    RTTI_METHOD(GetSetting);
    RTTI_METHOD(SetImpairment);
    RTTI_METHOD(SetDisplayName);
    RTTI_METHOD(SetBridge);
    RTTI_METHOD(ActivateTimeField);
    RTTI_METHOD(CancelTimeField);
    RTTI_METHOD(GetWorldRate);
    RTTI_METHOD(GetLocalRate);
    RTTI_METHOD(IsActivatingTimeField);
    RTTI_METHOD(GetAnimStatus);
    RTTI_METHOD(StartAnimRecording);
    RTTI_METHOD(DumpAnimFunctions);
    RTTI_METHOD(MirrorAnimationsTo);
    RTTI_METHOD(DumpAnimGraphs);
    RTTI_METHOD(SetAnimApplyVia);
    RTTI_METHOD(GetAnimApplyVia);
    RTTI_METHOD(ApplyAnimTest);
    RTTI_METHOD(SetPuppetAI);
    RTTI_METHOD(SetSwitchOffComponents);
    RTTI_METHOD(GetSwitchOffComponents);
    RTTI_METHOD(SetMotionInputs);
    RTTI_METHOD(GetMotionInputs);
    RTTI_METHOD(SetMotionFeature);
    RTTI_METHOD(GetMotionFeature);
    RTTI_METHOD(SetTppFeature);
    RTTI_METHOD(GetTppFeature);
    RTTI_METHOD(OnBodyInitialize);
    RTTI_METHOD(SetBodyOptions);
    RTTI_METHOD(GetBodyOptions);
    RTTI_METHOD(ApplyMyLook);
    RTTI_METHOD(SetLookOptions);
    RTTI_METHOD(GetLookOptions);
    RTTI_METHOD(SetPuppetLooks);
    RTTI_METHOD(GetPuppetLooks);
    RTTI_METHOD(PlaceEntity);
    RTTI_METHOD(GetPlacementError);
    RTTI_METHOD(GetPlacementMethod);
    RTTI_METHOD(GetPlacementMethodName);
    RTTI_METHOD(SetEntityComponents);
    RTTI_METHOD(SetSpawnTemplate);
});
