#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <RED4ext/RED4ext.hpp>
#include <RedLib.hpp>

#include "core/AnimInput.hpp"

namespace coop::plugin
{
// Captures the animation inputs the game applies to the local V, and applies inputs to a puppet
// (docs/01-architecture.md §4, "direct drive").
//
// Capture: the game's scripts feed V's animation graph through a few native functions (CD PROJEKT's leftover
// replication calls such as GameObject.ReplicateAnimFeature, and the player state machine's
// SetAnimationParameter* family). Their native handlers are looked up in RTTI and routed through ours: we read the
// arguments, keep the ones aimed at the local V, and call the original unchanged. Nothing is patched in the game's
// code; the handler table entries are restored on unload.
//
// Apply: a puppet built from the third-person V records uses the same animation setup as V, so the same inputs
// (features, float/int/bool/vector inputs, events) are applied to its animation controller.
class AnimCapture
{
public:
    static AnimCapture& Get();

    // Finds the capture points and routes them through us. Safe to call repeatedly; does the work once.
    void Install();
    // Puts the original handlers back (plugin unload).
    void Uninstall();

    void SetEnabled(bool aEnabled);
    [[nodiscard]] bool Enabled() const;

    // What changed since the last call (latest value per input, events in order). Two independent consumers:
    // the network session and the dev panel's mirror test.
    void DrainForNetwork(std::vector<AnimInput>& aOut);
    void DrainForMirror(std::vector<AnimInput>& aOut);
    void SetMirrorActive(bool aActive);
    // The local V (nullptr while there is none); only its inputs are captured.
    void SetLocalPlayer(RED4ext::IScriptable* aPlayer);

    // Writes every captured input with readable names to aFile for aSeconds.
    void StartRecording(float aSeconds, const std::filesystem::path& aFile);
    // Lists every RTTI function whose name mentions animation inputs or replication (native or script, params) to
    // aFile; returns how many. For finding capture points by hand.
    int DumpFunctions(const std::filesystem::path& aFile) const;

    // One paragraph for the dev panel.
    [[nodiscard]] std::string Status() const;

    // Called by the hooks.
    struct Hook;
    void OnCall(Hook& aHook, void* aContext, RED4ext::CStackFrame& aFrame);

private:
    AnimCapture() = default;
    void Store(const AnimInput& aInput, const char* aFunction);
};

// How inputs are applied to a puppet: queued as the game's AnimInputSetter* / AnimExternalEvent events on the
// entity (what the game's AnimationControllerComponent helpers do; default), or by calling the animation
// controller's natives (ApplyFeature, SetInput*, PushEvent).
enum class AnimApplyVia : int
{
    Events = 0,
    Controller = 1,
};
void SetAnimApplyVia(AnimApplyVia aVia);
AnimApplyVia GetAnimApplyVia();

// Applies inputs to an entity (a puppet). Returns how many were applied.
int ApplyAnimInputs(Red::IScriptable* aEntity, const std::vector<AnimInput>& aInputs, std::string* aError = nullptr);
} // namespace coop::plugin
