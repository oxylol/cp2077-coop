#include "Test.hpp"
#include "protocol/Auth.hpp"
#include "protocol/Codec.hpp"
#include "protocol/Messages.hpp"

using namespace coop;

namespace
{
template<typename M>
bool RoundTrip(const M& aIn, M& aOut)
{
    const auto bytes = Encode(aIn);
    if (bytes.empty())
        return false;
    EnvelopeHeader header;
    if (!PeekHeader(bytes.data(), bytes.size(), header) || header.id != M::kId)
        return false;
    return Decode(bytes, aOut);
}
} // namespace

TEST_CASE("protocol: hello and handshake messages round-trip")
{
    msg::Hello hello;
    hello.modVersion = "0.1.0";
    hello.gameBuild = "3.0.80.51928";
    hello.exeSize = 123456789;
    hello.manifestHash[3] = 7;
    hello.clientId[15] = 9;
    hello.displayName = "Filip";
    msg::Hello helloOut;
    REQUIRE(RoundTrip(hello, helloOut));
    CHECK_EQ(helloOut.protocolVersion, kProtocolVersion);
    CHECK_EQ(helloOut.gameBuild, hello.gameBuild);
    CHECK_EQ(helloOut.exeSize, hello.exeSize);
    CHECK(helloOut.manifestHash == hello.manifestHash);
    CHECK(helloOut.clientId == hello.clientId);
    CHECK_EQ(helloOut.displayName, hello.displayName);

    msg::Challenge challenge;
    challenge.salt[0] = 1;
    challenge.nonce[31] = 2;
    challenge.passwordRequired = true;
    challenge.iterations = 1000;
    msg::Challenge challengeOut;
    REQUIRE(RoundTrip(challenge, challengeOut));
    CHECK(challengeOut.salt == challenge.salt);
    CHECK(challengeOut.nonce == challenge.nonce);
    CHECK(challengeOut.passwordRequired);
    CHECK_EQ(challengeOut.iterations, 1000u);

    msg::Reject reject{RejectReason::BadPassword, "nope"};
    msg::Reject rejectOut;
    REQUIRE(RoundTrip(reject, rejectOut));
    CHECK(rejectOut.reason == RejectReason::BadPassword);
    CHECK_EQ(rejectOut.detail, "nope");

    msg::JoinAccept accept;
    accept.peer = 2;
    accept.stateRateHz = 30;
    accept.roster.push_back({0, "Host", 1, 0});
    accept.roster.push_back({1, "Jackie", 1, 42});
    msg::JoinAccept acceptOut;
    REQUIRE(RoundTrip(accept, acceptOut));
    CHECK_EQ(acceptOut.peer, 2);
    REQUIRE(acceptOut.roster.size() == 2);
    CHECK_EQ(acceptOut.roster[1].name, "Jackie");
    CHECK_EQ(acceptOut.roster[1].rttMs, 42);
}

TEST_CASE("protocol: player state round-trip and size")
{
    msg::PlayerState state;
    state.peer = 1;
    state.seq = 77;
    state.sessionTimeUs = 123'456'789;
    state.position = {-1520.25f, 1180.5f, 22.75f};
    state.velocity = {3.5f, -1.25f, 0.0f};
    state.yaw = 271.5f;
    state.pitch = -10.0f;
    state.locomotion = 2;
    state.flags = msg::PlayerStateFlags::Crouching;
    state.rate = 0.25f;

    const auto bytes = Encode(state);
    REQUIRE(!bytes.empty());
    // Header 4 + peer 1 + seq 4 + time 8 + field 2 + position ~7.5 + velocity ~5 + angles ~3.3 + 2 + rate
    CHECK(bytes.size() <= 40);

    msg::PlayerState out;
    REQUIRE(Decode(bytes, out));
    CHECK_EQ(out.peer, 1);
    CHECK_EQ(out.seq, 77u);
    CHECK_EQ(out.sessionTimeUs, state.sessionTimeUs);
    CHECK_NEAR(out.position.x, state.position.x, 0.01);
    CHECK_NEAR(out.position.y, state.position.y, 0.01);
    CHECK_NEAR(out.position.z, state.position.z, 0.01);
    CHECK_NEAR(out.velocity.x, 3.5, 0.02);
    CHECK_NEAR(out.yaw, 271.5, 0.01);
    CHECK_NEAR(out.rate, 0.25, 0.001);
    CHECK_EQ(out.locomotion, 2);
    CHECK_EQ(out.flags, msg::PlayerStateFlags::Crouching);
}

TEST_CASE("protocol: appearance with equipment list")
{
    msg::PlayerAppearance appearance;
    appearance.peer = 3;
    appearance.bodyGender = 1;
    appearance.customizationState.assign(5000, 0xAB);
    appearance.equipment = {"Items.Preset_Lexington_Default", "Items.SQ031_Samurai_Jacket"};
    msg::PlayerAppearance out;
    REQUIRE(RoundTrip(appearance, out));
    CHECK_EQ(out.bodyGender, 1);
    CHECK(out.customizationState == appearance.customizationState);
    CHECK(out.equipment == appearance.equipment);
}

TEST_CASE("protocol: wrong id and garbage are rejected")
{
    const auto bytes = Encode(msg::Heartbeat{5});
    msg::Chat chat;
    CHECK(!Decode(bytes, chat)); // id mismatch

    std::vector<uint8_t> garbage = {0x01, 0x00, 0x00, 0x00, 0xFF};
    msg::Hello hello;
    CHECK(!Decode(garbage, hello)); // truncated payload

    msg::Chat tooLong;
    tooLong.text.assign(msg::kMaxChatLength + 1, 'a');
    CHECK(Encode(tooLong).empty());
}

TEST_CASE("auth: proof depends on password, nonce and client id")
{
    std::array<uint8_t, 16> salt{};
    salt[0] = 42;
    std::array<uint8_t, 32> nonce{};
    nonce[5] = 1;
    Uuid client{};
    client[0] = 3;

    const auto key = auth::DeriveKey("secret", salt, 1000);
    const auto proof = auth::Proof(key, nonce, client);
    CHECK(proof == auth::Proof(auth::DeriveKey("secret", salt, 1000), nonce, client));
    CHECK(!(proof == auth::Proof(auth::DeriveKey("Secret", salt, 1000), nonce, client)));

    auto otherNonce = nonce;
    otherNonce[5] = 2;
    CHECK(!(proof == auth::Proof(key, otherNonce, client)));

    auto otherClient = client;
    otherClient[0] = 4;
    CHECK(!(proof == auth::Proof(key, nonce, otherClient)));
}
