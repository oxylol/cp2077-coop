#pragma once

#include <cstdint>
#include <deque>

#include "core/Math.hpp"
#include "core/Types.hpp"

namespace coop
{
// One snapshot of a vehicle as sent by the machine that simulates it (docs/02-systems.md §11).
struct VehicleSample
{
    TimeUs time = 0; // session time
    Vec3 position;
    Quat orientation;
    Vec3 velocity;
    float steer = 0.0f;    // -1 left .. 1 right
    float throttle = 0.0f; // -1 reverse .. 1 forward
    float brake = 0.0f;    // 0 .. 1
    uint8_t flags = 0;     // see VehicleFlags in protocol/Messages.hpp
};

// Snapshot buffer for one remote vehicle; same rules as InterpolationBuffer (core/Interpolation.hpp), with
// rotation interpolated as a quaternion because vehicles pitch, roll and flip.
class VehicleBuffer
{
public:
    static constexpr size_t kMaxSamples = 64;
    static constexpr TimeUs kMaxExtrapolationUs = 250 * kUsPerMs;

    bool Push(const VehicleSample& aSample);
    void Clear() { m_samples.clear(); }

    enum class Result
    {
        Empty,
        Interpolated,
        Extrapolated,
        Held,
        BeforeFirst,
    };

    Result Sample(TimeUs aRenderTime, VehicleSample& aOut) const;

    [[nodiscard]] size_t Size() const { return m_samples.size(); }

private:
    std::deque<VehicleSample> m_samples;
};
} // namespace coop
