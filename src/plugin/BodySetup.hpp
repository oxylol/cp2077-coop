#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <RED4ext/RED4ext.hpp>
#include <RedLib.hpp>

namespace coop::plugin
{
// Prepares puppet bodies while they are being built (Codeware's Entity/Initialize event, for entities tagged
// Cp2077Coop.Puppet or CoopMirror), before their components start.
//
// Round I: the bodies spawned from PlayerPuppet records (Character.TPP_Player, the photo mode puppet, the replacer
// base) use V's own animation graph and animate with V's captured inputs, but show only a neck; the cutscene
// lookalike looks like V (its gameImpostorComponent copies the local V) but its graph doesn't take V's inputs.
// addImpostor gives a player body (PlayerPuppet) without an impostor one, set up like the lookalike's (character
// replica, with head); round J: no visible effect, so off by default. (0.5.5-0.5.7 could also give a body V's root
// animation graph; on the NPC bodies, the only ones where that changed anything, it crashed the game, round L.)
// borrowAnimsets (round N): an NPC body (the cutscene lookalike) gets the local V's gameplay animation sets added
// to its root animated component, for the case that its graph doesn't walk because it has no walk animations.
struct BodyOptions
{
    bool addImpostor = false;
    bool borrowAnimsets = false;
};

class BodySetup
{
public:
    static BodySetup& Get();

    void SetOptions(const BodyOptions& aOptions);
    [[nodiscard]] BodyOptions Options() const;

    // Main thread: registers the Entity/Initialize callback with Codeware once it exists (aTarget receives it as
    // OnBodyInitialize). True once registered with both tag targets. Bodies are only prepared after that, so an
    // untargeted handler (which Codeware runs for every entity) never changes anything.
    bool EnsureRegistered(const Red::Handle<Red::IScriptable>& aTarget);
    [[nodiscard]] bool IsRegistered() const;
    // Main thread, several times a second: the local V, which is never prepared itself, and (when it changes)
    // the gameplay animation sets it has, for borrowAnimsets.
    void RememberPlayer(Red::IScriptable* aPlayer);
    // Main thread, when the world is detached.
    void ForgetPlayer();
    // From the callback (any thread): the body being built.
    void Prepare(Red::IScriptable* aEntity);

    [[nodiscard]] std::string Status() const;

private:
    BodySetup() = default;

    mutable std::mutex m_mutex;
    BodyOptions m_options;
    bool m_attempted = false; // registration tried and finished (successfully or for good)
    std::string m_registerError;
    std::string m_lastProblem;
    std::atomic<bool> m_active{false};
    std::atomic<RED4ext::IScriptable*> m_localPlayer{nullptr};
    std::atomic<uint64_t> m_prepared{0};
    std::atomic<uint64_t> m_impostorsAdded{0};
    std::atomic<uint64_t> m_animsetsBorrowed{0};
    std::vector<std::pair<uint64_t, uint8_t>> m_playerAnimsets; // path hash, priority
    RED4ext::IScriptable* m_animsetsOf = nullptr;              // whose sets those are (main thread only)
};
} // namespace coop::plugin
