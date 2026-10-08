#include <string>

#include "Test.hpp"
#include "core/BitStream.hpp"
#include "core/ClockSync.hpp"
#include "core/Crypto.hpp"
#include "core/Interpolation.hpp"
#include "core/Quantize.hpp"

using namespace coop;

namespace
{
std::string Hex(const Hash256& aHash)
{
    return crypto::ToHex(aHash.data(), aHash.size());
}

std::string Hex(const std::vector<uint8_t>& aBytes)
{
    return crypto::ToHex(aBytes.data(), aBytes.size());
}
} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Bit stream

TEST_CASE("bitstream: mixed fields round-trip")
{
    WriteStream out;
    uint32_t bits5 = 19;
    bool flag = true;
    uint8_t u8 = 200;
    uint16_t u16 = 54321;
    uint32_t u32 = 0xDEADBEEF;
    uint64_t u64 = 0x0123456789ABCDEFull;
    int64_t i64 = -1234567890123;
    int32_t ranged = -7;
    float f = 3.25f;
    std::string text = "Night City";
    std::vector<uint8_t> blob = {1, 2, 3, 250};

    REQUIRE(out.Bits(bits5, 5) && out.Bool(flag) && out.U8(u8) && out.U16(u16) && out.U32(u32) && out.U64(u64)
            && out.I64(i64) && out.IntRange(ranged, -10, 10) && out.Float(f) && out.String(text, 32)
            && out.Blob(blob, 16));
    const auto bytes = out.Finish();

    ReadStream in(bytes.data(), bytes.size());
    uint32_t rBits5 = 0;
    bool rFlag = false;
    uint8_t rU8 = 0;
    uint16_t rU16 = 0;
    uint32_t rU32 = 0;
    uint64_t rU64 = 0;
    int64_t rI64 = 0;
    int32_t rRanged = 0;
    float rF = 0.0f;
    std::string rText;
    std::vector<uint8_t> rBlob;
    REQUIRE(in.Bits(rBits5, 5) && in.Bool(rFlag) && in.U8(rU8) && in.U16(rU16) && in.U32(rU32) && in.U64(rU64)
            && in.I64(rI64) && in.IntRange(rRanged, -10, 10) && in.Float(rF) && in.String(rText, 32)
            && in.Blob(rBlob, 16));

    CHECK_EQ(rBits5, bits5);
    CHECK_EQ(rFlag, flag);
    CHECK_EQ(rU8, u8);
    CHECK_EQ(rU16, u16);
    CHECK_EQ(rU32, u32);
    CHECK_EQ(rU64, u64);
    CHECK_EQ(rI64, i64);
    CHECK_EQ(rRanged, ranged);
    CHECK_EQ(rF, f);
    CHECK_EQ(rText, text);
    CHECK(rBlob == blob);
}

TEST_CASE("bitstream: truncated input fails cleanly")
{
    WriteStream out;
    uint64_t value = 42;
    std::string text = "hello";
    out.U64(value);
    out.String(text, 64);
    auto bytes = out.Finish();
    bytes.resize(bytes.size() - 2);

    ReadStream in(bytes.data(), bytes.size());
    uint64_t rValue = 0;
    std::string rText;
    CHECK(in.U64(rValue));
    CHECK(!in.String(rText, 64));
}

TEST_CASE("bitstream: limits are enforced")
{
    WriteStream out;
    std::string tooLong(40, 'x');
    CHECK(!out.String(tooLong, 32));
    int32_t outOfRange = 11;
    CHECK(!out.IntRange(outOfRange, -10, 10));

    // A length prefix above the limit is rejected on read.
    WriteStream crafted;
    uint32_t length = 31;
    crafted.Bits(length, BitsRequired(31));
    const auto bytes = crafted.Finish();
    ReadStream in(bytes.data(), bytes.size());
    std::string text;
    CHECK(!in.String(text, 31)); // claims 31 bytes but none follow
}

TEST_CASE("quantize: position, yaw and velocity precision")
{
    const Vec3 positions[] = {{-1234.567f, 2345.678f, 123.456f}, {10000.0f, -10000.0f, -500.0f}, {0.0f, 0.0f, 0.0f}};
    for (const auto& original : positions)
    {
        WriteStream out;
        Vec3 value = original;
        float yaw = 359.99f;
        float pitch = -12.3f;
        Vec3 velocity{12.34f, -56.78f, 0.5f};
        REQUIRE(quant::Position(out, value) && quant::Yaw(out, yaw) && quant::Pitch(out, pitch)
                && quant::Velocity(out, velocity));
        const auto bytes = out.Finish();

        ReadStream in(bytes.data(), bytes.size());
        Vec3 rValue;
        float rYaw = 0.0f;
        float rPitch = 0.0f;
        Vec3 rVelocity;
        REQUIRE(quant::Position(in, rValue) && quant::Yaw(in, rYaw) && quant::Pitch(in, rPitch)
                && quant::Velocity(in, rVelocity));

        CHECK_NEAR(rValue.x, original.x, 0.006);
        CHECK_NEAR(rValue.y, original.y, 0.006);
        CHECK_NEAR(rValue.z, original.z, 0.005);
        CHECK(std::fabs(DeltaDegrees(rYaw, 359.99f)) < 0.01f);
        CHECK_NEAR(rPitch, -12.3f, 0.1);
        CHECK_NEAR(rVelocity.x, 12.34f, 0.02);
        CHECK_NEAR(rVelocity.y, -56.78f, 0.02);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Crypto (known-answer tests)

TEST_CASE("crypto: SHA-256 test vectors")
{
    const std::string abc = "abc";
    CHECK_EQ(Hex(crypto::Sha256::Hash(abc.data(), abc.size())),
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_EQ(Hex(crypto::Sha256::Hash("", 0)), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string twoBlocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK_EQ(Hex(crypto::Sha256::Hash(twoBlocks.data(), twoBlocks.size())),
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // Incremental updates match a one-shot hash.
    crypto::Sha256 incremental;
    for (char c : twoBlocks)
        incremental.Update(&c, 1);
    CHECK_EQ(Hex(incremental.Final()), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("crypto: HMAC-SHA256 RFC 4231")
{
    const std::vector<uint8_t> key1(20, 0x0b);
    const std::string data1 = "Hi There";
    CHECK_EQ(Hex(crypto::HmacSha256(key1.data(), key1.size(), reinterpret_cast<const uint8_t*>(data1.data()),
                                    data1.size())),
             "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    const std::string key2 = "Jefe";
    const std::string data2 = "what do ya want for nothing?";
    CHECK_EQ(Hex(crypto::HmacSha256(reinterpret_cast<const uint8_t*>(key2.data()), key2.size(),
                                    reinterpret_cast<const uint8_t*>(data2.data()), data2.size())),
             "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST_CASE("crypto: PBKDF2-HMAC-SHA256 vectors")
{
    const std::string salt = "salt";
    const auto* s = reinterpret_cast<const uint8_t*>(salt.data());
    CHECK_EQ(Hex(crypto::Pbkdf2Sha256("password", s, salt.size(), 1, 32)),
             "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    CHECK_EQ(Hex(crypto::Pbkdf2Sha256("password", s, salt.size(), 2, 32)),
             "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
    CHECK_EQ(Hex(crypto::Pbkdf2Sha256("password", s, salt.size(), 4096, 32)),
             "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
}

// ---------------------------------------------------------------------------------------------------------------------
// Clock sync

TEST_CASE("clocksync: converges under jitter and ignores slow samples")
{
    ClockSync sync;
    const TimeUs trueOffset = 5'000'000; // host session clock is 5 s ahead of the local clock
    uint32_t seed = 12345;
    auto random = [&seed]()
    {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<TimeUs>(seed % 40'000); // 0..40 ms of queuing delay
    };

    TimeUs local = 1'000'000;
    for (int i = 0; i < 40; ++i)
    {
        const TimeUs send = local;
        const TimeUs up = 30'000 + random();   // one-way base 30 ms plus jitter
        const TimeUs down = 30'000 + random();
        const TimeUs hostTime = send + up + trueOffset;
        const TimeUs recv = send + up + down;
        sync.AddSample(send, hostTime, recv);
        local += 100'000;
    }

    REQUIRE(sync.HasEstimate());
    // Asymmetric jitter limits accuracy to half the jitter of the fastest samples.
    CHECK_NEAR(static_cast<double>(sync.OffsetUs()), static_cast<double>(trueOffset), 10'000.0);
    CHECK(sync.RttUs() >= 60'000 && sync.RttUs() < 80'000);
}

// ---------------------------------------------------------------------------------------------------------------------
// Interpolation

TEST_CASE("interpolation: lerp, extrapolate, hold, yaw wrap")
{
    InterpolationBuffer buffer;
    PoseSample a;
    a.time = 1'000'000;
    a.position = {0.0f, 0.0f, 0.0f};
    a.velocity = {10.0f, 0.0f, 0.0f};
    a.yaw = 350.0f;
    PoseSample b = a;
    b.time = 1'100'000;
    b.position = {1.0f, 0.0f, 0.0f};
    b.yaw = 10.0f;
    CHECK(buffer.Push(a));
    CHECK(buffer.Push(b));
    CHECK(!buffer.Push(b)); // duplicate

    PoseSample out;
    CHECK(buffer.Sample(1'050'000, out) == InterpolationBuffer::Result::Interpolated);
    CHECK_NEAR(out.position.x, 0.5, 1e-4);
    CHECK_NEAR(DeltaDegrees(out.yaw, 0.0f), 0.0, 1e-3); // 350 -> 10 passes through 0

    CHECK(buffer.Sample(1'150'000, out) == InterpolationBuffer::Result::Extrapolated);
    CHECK_NEAR(out.position.x, 1.5, 1e-4);

    CHECK(buffer.Sample(2'000'000, out) == InterpolationBuffer::Result::Held);
    CHECK_NEAR(out.position.x, 3.0, 1e-4); // capped at 200 ms of extrapolation

    CHECK(buffer.Sample(500'000, out) == InterpolationBuffer::Result::BeforeFirst);

    // A late sample in the middle is inserted in order.
    PoseSample late = a;
    late.time = 1'050'000;
    late.position = {0.7f, 0.0f, 0.0f};
    CHECK(buffer.Push(late));
    CHECK(buffer.Sample(1'050'000, out) == InterpolationBuffer::Result::Interpolated);
    CHECK_NEAR(out.position.x, 0.7, 1e-4);
}

TEST_CASE("interpolation: adaptive delay grows with jitter")
{
    AdaptiveDelay steady(33'333);
    AdaptiveDelay jittery(33'333);
    uint32_t seed = 7;
    for (int i = 0; i < 300; ++i)
    {
        const TimeUs sent = i * 33'333;
        steady.OnArrival(sent, sent + 50'000);
        seed = seed * 1664525u + 1013904223u;
        jittery.OnArrival(sent, sent + 50'000 + static_cast<TimeUs>(seed % 60'000));
    }
    CHECK(steady.DelayUs() >= 66'000 && steady.DelayUs() <= 72'000);
    CHECK(jittery.DelayUs() > steady.DelayUs() + 20'000);
    CHECK(jittery.DelayUs() <= 250'000);
}
