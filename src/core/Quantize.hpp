#pragma once

#include "core/BitStream.hpp"
#include "core/Math.hpp"

namespace coop::quant
{
// World bounds for absolute position quantization. Night City plus the Badlands fit comfortably inside
// ±10.5 km horizontally; heights from below sea level to the tallest towers fit in ±1 km.
// [VERIFY] against the playable map extents in M1 (cells replace absolute quantization there).
inline constexpr float kWorldXYMin = -10485.76f;
inline constexpr float kWorldXYMax = 10485.76f;
inline constexpr int kWorldXYBits = 21; // ~1.0 cm
inline constexpr float kWorldZMin = -1024.0f;
inline constexpr float kWorldZMax = 1024.0f;
inline constexpr int kWorldZBits = 18; // ~0.8 cm

inline constexpr int kYawBits = 16;   // ~0.0055 degrees
inline constexpr int kPitchBits = 10; // ~0.18 degrees

inline constexpr float kVelocityMax = 128.0f; // m/s, covers fast vehicles
inline constexpr int kVelocityBits = 13;      // ~3 cm/s

inline constexpr int kRateBits = 10; // personal time rate in [0, 1]

template<typename S>
bool Position(S& aStream, Vec3& aValue)
{
    return aStream.Quantized(aValue.x, kWorldXYMin, kWorldXYMax, kWorldXYBits)
        && aStream.Quantized(aValue.y, kWorldXYMin, kWorldXYMax, kWorldXYBits)
        && aStream.Quantized(aValue.z, kWorldZMin, kWorldZMax, kWorldZBits);
}

template<typename S>
bool Yaw(S& aStream, float& aDegrees)
{
    float wrapped = WrapDegrees(aDegrees);
    // 360 maps to the same code as 0 after wrapping on read.
    if (!aStream.Quantized(wrapped, 0.0f, 360.0f, kYawBits))
        return false;
    if constexpr (S::kIsReading)
        aDegrees = WrapDegrees(wrapped);
    return true;
}

template<typename S>
bool Pitch(S& aStream, float& aDegrees)
{
    return aStream.Quantized(aDegrees, -90.0f, 90.0f, kPitchBits);
}

template<typename S>
bool Velocity(S& aStream, Vec3& aValue)
{
    return aStream.Quantized(aValue.x, -kVelocityMax, kVelocityMax, kVelocityBits)
        && aStream.Quantized(aValue.y, -kVelocityMax, kVelocityMax, kVelocityBits)
        && aStream.Quantized(aValue.z, -kVelocityMax, kVelocityMax, kVelocityBits);
}

template<typename S>
bool Rate(S& aStream, float& aRate)
{
    return aStream.Quantized(aRate, 0.0f, 1.0f, kRateBits);
}

// Rotation, "smallest three": drop the largest component (its sign is made positive), send which one it was
// (2 bits) and the other three in [-1/sqrt2, 1/sqrt2]. 15 bits each: ~0.01 degree worst case.
inline constexpr int kQuatComponentBits = 15;
inline constexpr float kQuatComponentMax = 0.70710678f;

template<typename S>
bool Orientation(S& aStream, Quat& aValue)
{
    uint32_t largest = 0;
    float c[4] = {aValue.x, aValue.y, aValue.z, aValue.w};
    if constexpr (!S::kIsReading)
    {
        const Quat q = aValue.Normalized();
        c[0] = q.x;
        c[1] = q.y;
        c[2] = q.z;
        c[3] = q.w;
        for (uint32_t i = 1; i < 4; ++i)
        {
            if (std::abs(c[i]) > std::abs(c[largest]))
                largest = i;
        }
        if (c[largest] < 0.0f)
        {
            for (auto& component : c)
                component = -component;
        }
    }
    if (!aStream.Bits(largest, 2))
        return false;

    float rest[3];
    int index = 0;
    for (uint32_t i = 0; i < 4; ++i)
    {
        if (i != largest)
            rest[index++] = c[i];
    }
    for (auto& component : rest)
    {
        if (!aStream.Quantized(component, -kQuatComponentMax, kQuatComponentMax, kQuatComponentBits))
            return false;
    }

    if constexpr (S::kIsReading)
    {
        const float sum = rest[0] * rest[0] + rest[1] * rest[1] + rest[2] * rest[2];
        float out[4];
        index = 0;
        for (uint32_t i = 0; i < 4; ++i)
            out[i] = i == largest ? std::sqrt(std::max(0.0f, 1.0f - sum)) : rest[index++];
        aValue = Quat{out[0], out[1], out[2], out[3]}.Normalized();
    }
    return true;
}

// Value in [-1, 1] (steering, throttle).
template<typename S>
bool SignedUnit(S& aStream, float& aValue, int aBits = 8)
{
    return aStream.Quantized(aValue, -1.0f, 1.0f, aBits);
}

// Value in [0, 1] (brake).
template<typename S>
bool Unit(S& aStream, float& aValue, int aBits = 6)
{
    return aStream.Quantized(aValue, 0.0f, 1.0f, aBits);
}
} // namespace coop::quant
