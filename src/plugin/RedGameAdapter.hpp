#pragma once

#include <array>
#include <chrono>
#include <map>
#include <string>

#include <RED4ext/RED4ext.hpp>
#include <RedLib.hpp>

#include "client/GameAdapter.hpp"
#include "core/AnimMotion.hpp"

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
    // Animation inputs: captured from the local V (AnimCapture), applied to puppets.
    void CaptureAnimInputs(std::vector<AnimInput>& aOut) override;
    void ApplyRemoteAnimInputs(PeerId aPeer, const std::vector<AnimInput>& aInputs, bool aFull) override;
    void SetAnimOptions(bool aCapture, bool aApply);
    [[nodiscard]] std::string AnimApplyStatus() const;
    // Graph inputs fed from each puppet's pose (speed, direction, vertical speed, turn rate, moving), by name; empty
    // names are not sent (core/AnimMotion.hpp, coop.ini [anim]).
    void SetMotionInputs(const std::array<std::string, 5>& aNames);
    [[nodiscard]] std::string MotionStatus() const;
    // Also send the motion values as V's graph's own walking feature (playerLocomotion; core/AnimMotion.hpp).
    void SetMotionFeature(bool aOn) { m_motionFeature = aOn; }
    [[nodiscard]] bool MotionFeature() const { return m_motionFeature; }
    // Also tell player bodies to animate as third person (TPPRepresentation feature; core/AnimMotion.hpp).
    void SetTppFeature(bool aOn) { m_tppFeature = aOn; }
    [[nodiscard]] bool TppFeature() const { return m_tppFeature; }
    // The names in use, as set ("-name" when negated, "" when not sent).
    [[nodiscard]] const std::array<std::string, 5>& MotionNameText() const { return m_motionNameText; }
    // The dev panel's mirror body: the same motion inputs, worked out from where the body is placed each frame.
    // Returns how many inputs were applied.
    int DriveMirrorMotion(Red::IScriptable* aEntity);
    void ResetMirrorMotion();
    // The entity of a remote player's puppet (nullptr if none yet).
    Red::Handle<Red::IScriptable> PuppetEntity(PeerId aPeer) const;
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

    struct AnimApplyStats
    {
        uint64_t messages = 0;
        uint64_t applied = 0;
        uint64_t received = 0;
        std::string lastError;
    };
    std::map<PeerId, AnimApplyStats> m_animStats;
    bool m_animCapture = true;
    bool m_animApply = true;

    struct Motion
    {
        MotionTracker tracker;
        PositionVelocity velocity; // mirror only
        std::chrono::steady_clock::time_point last;
        bool started = false;
        uint64_t applied = 0;

        float Step(); // seconds since the previous step (0 on the first)
    };
    [[nodiscard]] MotionValues Signed(MotionValues aValues) const;
    // The inputs a placed body gets this frame from its motion values (named floats, and the walking feature).
    [[nodiscard]] std::vector<AnimInput> MotionInputs(const MotionValues& aValues) const;
    [[nodiscard]] bool SendsMotion() const
    {
        return m_motionNames.Any() || !m_motionExtras.empty() || m_motionFeature || m_tppFeature;
    }
    bool m_motionFeature = true;
    bool m_tppFeature = false; // round M: T-pose

    MotionInputNames m_motionNames;
    struct MotionExtra
    {
        size_t slot; // 0 speed, 1 direction, 2 vertical, 3 turn rate, 4 moving
        uint64_t name;
        float sign;
    };
    std::vector<MotionExtra> m_motionExtras; // second and later names of a value ("a+b")
    std::array<std::string, 5> m_motionNameText;
    std::array<float, 5> m_motionSigns{1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    std::map<PeerId, Motion> m_motion;
    Motion m_mirrorMotion;
};
} // namespace coop::plugin
