#pragma once

#include <cstdint>
#include <vector>

// Animation inputs: what the game feeds a character's animation graph (docs/01-architecture.md §4).
//
// The plugin captures the inputs the game applies to the local V (anim features, float/int/bool/vector inputs,
// events), the session sends them to the other players, and their games apply them to that player's puppet, which
// uses the same third-person V animation setup. Names are engine name hashes (CName, FNV-1a 64), identical on
// every machine, so nothing here needs to know what an input means.
namespace coop
{
enum class AnimInputKind : uint8_t
{
    Feature = 0, // an AnimFeature object: a class plus property values
    Float,
    Int,
    Bool,
    Vector,
    Event, // one-shot, no value
    Count,
};

// The value types an AnimFeature property can have here; anything else is skipped when capturing.
enum class AnimValueType : uint8_t
{
    Float = 0,
    Int,    // signed integers and enums
    Bool,
    Name,   // CName hash
    Vector, // 4 floats
    Count,
};

struct AnimValue
{
    AnimValueType type = AnimValueType::Float;
    float f[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // Float uses f[0], Vector all four
    int64_t i = 0;                         // Int, Bool (0/1), Name (hash bits)

    bool operator==(const AnimValue&) const = default;

    static AnimValue FromFloat(float aValue)
    {
        AnimValue value;
        value.type = AnimValueType::Float;
        value.f[0] = aValue;
        return value;
    }
    static AnimValue FromInt(int64_t aValue)
    {
        AnimValue value;
        value.type = AnimValueType::Int;
        value.i = aValue;
        return value;
    }
    static AnimValue FromBool(bool aValue)
    {
        AnimValue value;
        value.type = AnimValueType::Bool;
        value.i = aValue ? 1 : 0;
        return value;
    }
    static AnimValue FromName(uint64_t aHash)
    {
        AnimValue value;
        value.type = AnimValueType::Name;
        value.i = static_cast<int64_t>(aHash);
        return value;
    }
    static AnimValue FromVector(float aX, float aY, float aZ, float aW)
    {
        AnimValue value;
        value.type = AnimValueType::Vector;
        value.f[0] = aX;
        value.f[1] = aY;
        value.f[2] = aZ;
        value.f[3] = aW;
        return value;
    }

    template<typename S>
    bool Serialize(S& s, AnimValueType aType)
    {
        type = aType;
        switch (aType)
        {
        case AnimValueType::Float: return s.Float(f[0]);
        case AnimValueType::Vector: return s.Float(f[0]) && s.Float(f[1]) && s.Float(f[2]) && s.Float(f[3]);
        case AnimValueType::Bool:
        {
            bool flag = i != 0;
            if (!s.Bool(flag))
                return false;
            i = flag ? 1 : 0;
            return true;
        }
        case AnimValueType::Int:
        case AnimValueType::Name: return s.I64(i);
        default: return false;
        }
    }
};

struct AnimProp
{
    uint64_t name = 0; // property name hash
    AnimValue value;

    bool operator==(const AnimProp&) const = default;

    template<typename S>
    bool Serialize(S& s)
    {
        auto type = static_cast<uint8_t>(value.type);
        if (!s.U64(name) || !s.U8(type) || type >= static_cast<uint8_t>(AnimValueType::Count))
            return false;
        return value.Serialize(s, static_cast<AnimValueType>(type));
    }
};

inline constexpr int32_t kMaxAnimProps = 64;
inline constexpr int32_t kMaxAnimInputsPerMessage = 256;

struct AnimInput
{
    AnimInputKind kind = AnimInputKind::Float;
    uint64_t name = 0;         // the graph input (or event, or feature input) name hash
    uint64_t featureClass = 0; // Feature: class name hash of the AnimFeature
    AnimValue value;           // Float, Int, Bool, Vector
    std::vector<AnimProp> props; // Feature

    bool operator==(const AnimInput&) const = default;

    // Inputs replace each other per kind and name; events never do (each one fires once).
    [[nodiscard]] uint64_t Key() const { return name ^ (static_cast<uint64_t>(kind) << 56); }

    template<typename S>
    bool Serialize(S& s)
    {
        auto kindByte = static_cast<uint8_t>(kind);
        if (!s.U8(kindByte) || kindByte >= static_cast<uint8_t>(AnimInputKind::Count) || !s.U64(name))
            return false;
        kind = static_cast<AnimInputKind>(kindByte);
        switch (kind)
        {
        case AnimInputKind::Feature:
        {
            if (!s.U64(featureClass))
                return false;
            auto count = static_cast<int32_t>(props.size());
            if (!s.IntRange(count, 0, kMaxAnimProps))
                return false;
            if constexpr (S::kIsReading)
                props.resize(static_cast<size_t>(count));
            for (auto& prop : props)
            {
                if (!prop.Serialize(s))
                    return false;
            }
            return true;
        }
        case AnimInputKind::Float: return value.Serialize(s, AnimValueType::Float);
        case AnimInputKind::Int: return value.Serialize(s, AnimValueType::Int);
        case AnimInputKind::Bool: return value.Serialize(s, AnimValueType::Bool);
        case AnimInputKind::Vector: return value.Serialize(s, AnimValueType::Vector);
        case AnimInputKind::Event: return true;
        default: return false;
        }
    }
};
} // namespace coop
