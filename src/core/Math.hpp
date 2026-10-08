#pragma once

#include <algorithm>
#include <cmath>

namespace coop
{
struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float aX, float aY, float aZ)
        : x(aX)
        , y(aY)
        , z(aZ)
    {
    }

    constexpr Vec3 operator+(const Vec3& aOther) const { return {x + aOther.x, y + aOther.y, z + aOther.z}; }
    constexpr Vec3 operator-(const Vec3& aOther) const { return {x - aOther.x, y - aOther.y, z - aOther.z}; }
    constexpr Vec3 operator*(float aScale) const { return {x * aScale, y * aScale, z * aScale}; }
    constexpr Vec3& operator+=(const Vec3& aOther)
    {
        x += aOther.x;
        y += aOther.y;
        z += aOther.z;
        return *this;
    }

    [[nodiscard]] float Length() const { return std::sqrt(x * x + y * y + z * z); }
    [[nodiscard]] float Length2D() const { return std::sqrt(x * x + y * y); }
};

inline Vec3 Lerp(const Vec3& aA, const Vec3& aB, float aT)
{
    return aA + (aB - aA) * aT;
}

inline float Distance(const Vec3& aA, const Vec3& aB)
{
    return (aB - aA).Length();
}

// Wraps an angle in degrees into [0, 360).
inline float WrapDegrees(float aDegrees)
{
    float result = std::fmod(aDegrees, 360.0f);
    if (result < 0.0f)
        result += 360.0f;
    return result;
}

// Signed shortest difference b - a in degrees, in (-180, 180].
inline float DeltaDegrees(float aA, float aB)
{
    float delta = WrapDegrees(aB - aA);
    if (delta > 180.0f)
        delta -= 360.0f;
    return delta;
}

// Interpolates along the shortest arc.
inline float LerpDegrees(float aA, float aB, float aT)
{
    return WrapDegrees(aA + DeltaDegrees(aA, aB) * aT);
}

template<typename T>
constexpr T Clamp(T aValue, T aMin, T aMax)
{
    return std::min(std::max(aValue, aMin), aMax);
}

// Rotation quaternion (x, y, z, w), same layout as the game's Quaternion (i, j, k, r). World axes follow the
// game: Z up, yaw about Z, yaw 0 facing +Y.
struct Quat
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    static Quat FromYawDegrees(float aYaw)
    {
        const float half = aYaw * 0.5f * 3.14159265358979f / 180.0f;
        return {0.0f, 0.0f, std::sin(half), std::cos(half)};
    }

    [[nodiscard]] float Dot(const Quat& aOther) const { return x * aOther.x + y * aOther.y + z * aOther.z + w * aOther.w; }

    [[nodiscard]] Quat Normalized() const
    {
        const float length = std::sqrt(x * x + y * y + z * z + w * w);
        if (length <= 1e-8f)
            return {};
        return {x / length, y / length, z / length, w / length};
    }

    // Yaw about Z in degrees, [0, 360). Exact for pure yaw rotations; approximate when pitched or rolled.
    [[nodiscard]] float YawDegrees() const
    {
        const float siny = 2.0f * (w * z + x * y);
        const float cosy = 1.0f - 2.0f * (y * y + z * z);
        return WrapDegrees(std::atan2(siny, cosy) * 180.0f / 3.14159265358979f);
    }
};

// Normalized linear interpolation along the shorter arc; close enough to slerp for snapshot spacing.
inline Quat Nlerp(const Quat& aA, const Quat& aB, float aT)
{
    const float sign = aA.Dot(aB) < 0.0f ? -1.0f : 1.0f;
    Quat result{aA.x + (aB.x * sign - aA.x) * aT, aA.y + (aB.y * sign - aA.y) * aT, aA.z + (aB.z * sign - aA.z) * aT,
                aA.w + (aB.w * sign - aA.w) * aT};
    return result.Normalized();
}

// Angle between two rotations in degrees.
inline float AngleDegrees(const Quat& aA, const Quat& aB)
{
    const float dot = std::min(1.0f, std::abs(aA.Normalized().Dot(aB.Normalized())));
    return 2.0f * std::acos(dot) * 180.0f / 3.14159265358979f;
}
} // namespace coop
