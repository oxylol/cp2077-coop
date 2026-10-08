#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "core/TweakDbId.hpp"

namespace coop
{
// The items that dress a player's body, each with the attachment slot it sits in, as they travel in the
// appearance message (msg::PlayerAppearance::equipment, one string per item). CyberpunkMP sends item record names
// (TDBID.ToStringDEBUG); names aren't always known in a retail game, so here each entry is the slot and item
// TweakDBIDs as 16 hex digits each: "slot:item".
struct LookItem
{
    uint64_t slot = 0; // TweakDBID of the AttachmentSlots record
    uint64_t item = 0; // TweakDBID of the Items record

    bool operator==(const LookItem&) const = default;
};

// The part of a TweakDBID that names the record (CRC-32 and name length); the upper bits are a runtime offset.
constexpr uint64_t TweakDbName(uint64_t aId)
{
    return aId & 0xFF'FFFF'FFFFull;
}

inline std::string EncodeLookItem(const LookItem& aItem)
{
    char text[40];
    std::snprintf(text, sizeof(text), "%016llx:%016llx", static_cast<unsigned long long>(aItem.slot),
                  static_cast<unsigned long long>(aItem.item));
    return text;
}

inline bool DecodeLookItem(std::string_view aText, LookItem& aOut)
{
    if (aText.size() != 33 || aText[16] != ':')
        return false;
    const auto hex = [](std::string_view aDigits, uint64_t& aValue) {
        aValue = 0;
        for (const char c : aDigits)
        {
            uint64_t digit = 0;
            if (c >= '0' && c <= '9')
                digit = static_cast<uint64_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = static_cast<uint64_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                digit = static_cast<uint64_t>(c - 'A' + 10);
            else
                return false;
            aValue = (aValue << 4) | digit;
        }
        return true;
    };
    LookItem item;
    if (!hex(aText.substr(0, 16), item.slot) || !hex(aText.substr(17, 16), item.item) || item.slot == 0
        || item.item == 0)
        return false;
    aOut = item;
    return true;
}

inline std::vector<std::string> EncodeLookItems(const std::vector<LookItem>& aItems)
{
    std::vector<std::string> out;
    out.reserve(aItems.size());
    for (const auto& item : aItems)
        out.push_back(EncodeLookItem(item));
    return out;
}

// Entries that aren't "slot:item" (e.g. from an older build) are skipped.
inline std::vector<LookItem> DecodeLookItems(const std::vector<std::string>& aTexts)
{
    std::vector<LookItem> out;
    for (const auto& text : aTexts)
    {
        LookItem item;
        if (DecodeLookItem(text, item))
            out.push_back(item);
    }
    return out;
}

// What a player's own items become on a third-person body of the given gender (the dev panel's dressing, round K):
// the first-person head is swapped for the third-person head of that body, and a third-person head is added if
// the list has none, because a spawned player body shows only a neck without one (round I).
inline std::vector<LookItem> ThirdPersonLookItems(const std::vector<LookItem>& aItems, bool aFemale)
{
    constexpr uint64_t kTppHeadSlot = TweakDbId("AttachmentSlots.TppHead");
    constexpr uint64_t kFppHead = TweakDbId("Items.PlayerFppHead");
    constexpr uint64_t kMaleHead = TweakDbId("Items.PlayerMaTppHead");
    constexpr uint64_t kFemaleHead = TweakDbId("Items.PlayerWaTppHead");
    const uint64_t head = aFemale ? kFemaleHead : kMaleHead;

    std::vector<LookItem> out;
    bool hasHead = false;
    for (auto item : aItems)
    {
        const uint64_t name = TweakDbName(item.item);
        if (name == kFppHead || name == kMaleHead || name == kFemaleHead)
        {
            if (hasHead)
                continue;
            item.item = head;
            hasHead = true;
        }
        out.push_back(item);
    }
    if (!hasHead)
        out.push_back({kTppHeadSlot, head});
    return out;
}
} // namespace coop
