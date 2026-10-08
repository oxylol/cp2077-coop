#pragma once

#include <cstdint>
#include <deque>

#include "core/Math.hpp"
#include "core/Types.hpp"

namespace coop
{
struct PoseSample
{
    TimeUs time = 0; // session time (M0) or field world time (M1+), consistently per buffer
    Vec3 position;
    Vec3 velocity;
    float yaw = 0.0f;
    float pitch = 0.0f;
    uint8_t locomotion = 0;
    uint8_t flags = 0;
    float rate = 1.0f;
};

// Snapshot buffer for one remote entity. Samples are kept in time order; Sample() interpolates between
// the two samples bracketing the render time and dead-reckons a short way past the newest one.
class InterpolationBuffer
{
public:
    static constexpr size_t kMaxSamples = 64;
    static constexpr TimeUs kMaxExtrapolationUs = 200 * kUsPerMs;

    // Returns false for duplicates or samples older than the newest by more than the buffer span.
    bool Push(const PoseSample& aSample);
    void Clear() { m_samples.clear(); }

    enum class Result
    {
        Empty,
        Interpolated,
        Extrapolated, // past the newest sample, within kMaxExtrapolationUs
        Held,         // past the extrapolation limit: newest pose, zero velocity
        BeforeFirst,  // render time precedes all samples: oldest pose
    };

    Result Sample(TimeUs aRenderTime, PoseSample& aOut) const;

    [[nodiscard]] size_t Size() const { return m_samples.size(); }
    [[nodiscard]] TimeUs NewestTime() const { return m_samples.empty() ? 0 : m_samples.back().time; }

private:
    std::deque<PoseSample> m_samples;
};

// Picks an interpolation delay from observed arrival jitter:
// delay = clamp(2 * sendInterval + 2 * jitter, min, max). Arrival times and send times are both in
// session time, so their difference is network transit plus clock error; only its variation matters.
class AdaptiveDelay
{
public:
    AdaptiveDelay(TimeUs aSendIntervalUs = 33'333, TimeUs aMinUs = 70 * kUsPerMs, TimeUs aMaxUs = 250 * kUsPerMs);

    void OnArrival(TimeUs aSentAt, TimeUs aArrivedAt);
    [[nodiscard]] TimeUs DelayUs() const;
    [[nodiscard]] double JitterUs() const { return m_jitter; }

private:
    TimeUs m_sendInterval;
    TimeUs m_min;
    TimeUs m_max;
    bool m_hasTransit = false;
    double m_meanTransit = 0.0;
    double m_jitter = 0.0;
};
} // namespace coop
