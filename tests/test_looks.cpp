#include <string>
#include <vector>

#include "Test.hpp"
#include "core/LookItems.hpp"
#include "protocol/Codec.hpp"
#include "protocol/Messages.hpp"

using namespace coop;

TEST_CASE("looks: item entries round-trip as slot:item hex")
{
    const LookItem item{TweakDbId("AttachmentSlots.Chest"), TweakDbId("Items.Preset_Jacket_Example")};
    const auto text = EncodeLookItem(item);
    CHECK_EQ(text.size(), 33u);
    CHECK_EQ(text[16], ':');
    LookItem back;
    CHECK(DecodeLookItem(text, back));
    CHECK(back == item);

    // Upper-case hex is read too; anything else is not an entry.
    std::string upper = text;
    for (auto& c : upper)
        c = static_cast<char>(c >= 'a' && c <= 'f' ? c - 'a' + 'A' : c);
    CHECK(DecodeLookItem(upper, back));
    CHECK(back == item);
    CHECK(!DecodeLookItem("Items.Preset_Jacket_Example", back));
    CHECK(!DecodeLookItem(text.substr(0, 32), back));
    CHECK(!DecodeLookItem(std::string(16, '0') + ":" + std::string(16, '0'), back)); // zero ids
    std::string bad = text;
    bad[3] = 'g';
    CHECK(!DecodeLookItem(bad, back));
}

TEST_CASE("looks: lists skip entries from older builds")
{
    const std::vector<LookItem> items = {{TweakDbId("AttachmentSlots.Head"), TweakDbId("Items.Hat")},
                                         {TweakDbId("AttachmentSlots.Feet"), TweakDbId("Items.Boots")}};
    auto texts = EncodeLookItems(items);
    texts.insert(texts.begin() + 1, "Items.SomeOldName");
    const auto decoded = DecodeLookItems(texts);
    CHECK_EQ(decoded.size(), 2u);
    CHECK(decoded == items);
}

TEST_CASE("looks: the record name part of a TweakDBID drops the runtime offset")
{
    const uint64_t id = TweakDbId("Items.PlayerFppHead");
    CHECK_EQ(TweakDbName(id), id);
    CHECK_EQ(TweakDbName(id | (0xABCDEFull << 40)), id);
}

TEST_CASE("looks: a third-person body gets the third-person head of its gender")
{
    const uint64_t tppSlot = TweakDbId("AttachmentSlots.TppHead");
    const uint64_t chest = TweakDbId("AttachmentSlots.Chest");
    const uint64_t jacket = TweakDbId("Items.Jacket");

    // V in first person: the first-person head in TppHead becomes the third-person one, other items stay.
    const std::vector<LookItem> firstPerson = {{tppSlot, TweakDbId("Items.PlayerFppHead") | (5ull << 40)},
                                               {chest, jacket}};
    auto male = ThirdPersonLookItems(firstPerson, false);
    CHECK_EQ(male.size(), 2u);
    CHECK(male[0] == (LookItem{tppSlot, TweakDbId("Items.PlayerMaTppHead")}));
    CHECK(male[1] == (LookItem{chest, jacket}));
    auto female = ThirdPersonLookItems(firstPerson, true);
    CHECK(female[0] == (LookItem{tppSlot, TweakDbId("Items.PlayerWaTppHead")}));

    // No head at all: one is added. A head of the other gender is replaced, and only one head goes on.
    auto added = ThirdPersonLookItems({{chest, jacket}}, true);
    CHECK_EQ(added.size(), 2u);
    CHECK(added[1] == (LookItem{tppSlot, TweakDbId("Items.PlayerWaTppHead")}));
    auto twice = ThirdPersonLookItems({{tppSlot, TweakDbId("Items.PlayerMaTppHead")},
                                       {tppSlot, TweakDbId("Items.PlayerFppHead")}},
                                      true);
    CHECK_EQ(twice.size(), 1u);
    CHECK(twice[0] == (LookItem{tppSlot, TweakDbId("Items.PlayerWaTppHead")}));
}

TEST_CASE("looks: a full look fits the appearance message")
{
    // CyberpunkMP's saved default male state is 13 KB; 13 slots of items.
    msg::PlayerAppearance message;
    message.peer = 2;
    message.bodyGender = 1;
    message.customizationState.assign(16 * 1024, 0x5A);
    for (int i = 0; i < 13; ++i)
        message.equipment.push_back(EncodeLookItem({TweakDbId("AttachmentSlots.Chest") + static_cast<uint64_t>(i),
                                                    TweakDbId("Items.Jacket")}));
    const auto bytes = Encode(message);
    msg::PlayerAppearance back;
    CHECK(Decode(bytes, back));
    CHECK_EQ(back.customizationState.size(), message.customizationState.size());
    CHECK(DecodeLookItems(back.equipment).size() == 13u);
    CHECK_EQ(back.bodyGender, 1);
}
