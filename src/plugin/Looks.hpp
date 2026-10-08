#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <RED4ext/RED4ext.hpp>
#include <RedLib.hpp>

#include "core/LookItems.hpp"

namespace coop::plugin
{
// A player's look on a spawned body, the way CyberpunkMP does it (Tilted Phoques SRL; ported from
// code/client/App/World/AppearanceSystem.cpp and NetworkService.cpp, CyberpunkMP license, see LICENSE.md):
//
//  1. the body is marked third person in its persistent state (gamePuppetPS), which the game checks when it
//     resolves item appearances (&TPP) -- the first-person head swap of rounds K-M;
//  2. it gets the player's items, each into the slot it sits in on the player;
//  3. the player's character customization state (face, hair, skin, body: the character creator's choices),
//     serialized the way the game saves it, is read back into a new state, and its third-person head, face,
//     hair, beard, body and arms parts are applied to the body through the world's entity appearance changer.
//
// Differences from CyberpunkMP: the body is the player's own third-person template with V's animation graph
// (CyberpunkMP uses an NPC body driven by its own walk/sprint controller); items are read from the player's
// attachment slots and sent as TweakDBIDs (core/LookItems.hpp); the transaction system is called through its
// script natives (RTTI) instead of virtual-table slots from an older game version; engine addresses are looked up
// without RED4ext's terminate-on-failure, so a missing one switches that step off and says so.
struct LookOptions
{
    bool thirdPerson = true;   // step 1
    bool items = true;         // step 2
    bool customization = true; // step 3
};

struct Look
{
    bool female = false;
    std::vector<uint8_t> customization; // serialized gameuiCharacterCustomizationState (empty = none)
    std::vector<LookItem> items;
};

// Main thread. The local V from the player system (empty while there is none).
Red::Handle<Red::IScriptable> LocalPlayerHandle();

class Looks
{
public:
    static Looks& Get();

    void SetOptions(const LookOptions& aOptions);
    [[nodiscard]] LookOptions Options() const;

    // Main thread. The local V's look (customization state and the items in its look slots). Partial results are
    // kept: a V whose state can't be read still has its items. aError says what was missing.
    bool CaptureLocal(const Red::Handle<Red::IScriptable>& aPlayer, bool aFemale, Look& aOut, std::string& aError);

    // Main thread. Applies a look to a spawned body (a PlayerPuppet or NPC). Returns a one-line report, also
    // logged. Each step is logged before it runs, so a crash names its step.
    std::string Apply(const Red::Handle<Red::IScriptable>& aEntity, const Look& aLook);

    [[nodiscard]] std::string Status() const;

private:
    Looks() = default;

    bool ApplyItems(const Red::Handle<Red::IScriptable>& aEntity, const Look& aLook, std::string& aReport);
    bool ApplyCustomization(const Red::Handle<Red::IScriptable>& aEntity, const Look& aLook, std::string& aReport);

    mutable std::mutex m_mutex;
    LookOptions m_options;
    std::string m_lastReport;
    std::string m_lastCaptureError;
    std::atomic<uint64_t> m_applied{0};
    std::atomic<size_t> m_lastCaptureSize{0};
};
} // namespace coop::plugin
