#include "core/Interpolation.hpp"

#include <algorithm>
#include <cmath>

namespace coop
{
bool InterpolationBuffer::Push(const PoseSample& aSample)
{
    if (!m_samples.empty())
    {
        const TimeUs newest = m_samples.back().time;
        if (aSample.time == newest)
            return false;
        if (aSample.time < newest)
        {
            // Late arrival over an unreliable lane: insert in order if it still falls inside the buffer.
            if (aSample.time < m_samples.front().time)
                return false;
            auto it = std::lower_bound(m_samples.begin(), m_samples.end(), aSample.time,
                                       [](const PoseSample& aA, TimeUs aT) { return aA.time < aT; });
            if (it != m_samples.end() && it->time == aSample.time)
                return false;
            m_samples.insert(it, aSample);
            return true;
        }
    }

    m_samples.push_back(aSample);
    while (m_samples.size() > kMaxSamples)
        m_samples.pop_front();
    return true;
}

InterpolationBuffer::Result InterpolationBuffer::Sample(TimeUs aRenderTime, PoseSample& aOut) const
{
    if (m_samples.empty())
        return Result::Empty;

    const auto& oldest = m_samples.front();
    const auto& newest = m_samples.back();

    if (aRenderTime <= oldest.time)
    {
        aOut = oldest;
        aOut.time = aRenderTime;
        return Result::BeforeFirst;
    }

    if (aRenderTime >= newest.time)
    {
        const TimeUs ahead = aRenderTime - newest.time;
        aOut = newest;
        aOut.time = aRenderTime;
        if (ahead <= kMaxExtrapolationUs)
        {
            aOut.position = newest.position + newest.velocity * static_cast<float>(ToSeconds(ahead));
            return Result::Extrapolated;
        }
        aOut.position = newest.position + newest.velocity * static_cast<float>(ToSeconds(kMaxExtrapolationUs));
        aOut.velocity = {};
        return Result::Held;
    }

    auto upper = std::upper_bound(m_samples.begin(), m_samples.end(), aRenderTime,
                                  [](TimeUs aT, const PoseSample& aS) { return aT < aS.time; });
    const auto& b = *upper;
    const auto& a = *(upper - 1);

    const double span = static_cast<double>(b.time - a.time);
    const float t = span > 0.0 ? static_cast<float>(static_cast<double>(aRenderTime - a.time) / span) : 0.0f;

    aOut.time = aRenderTime;
    aOut.position = Lerp(a.position, b.position, t);
    aOut.velocity = Lerp(a.velocity, b.velocity, t);
    aOut.yaw = LerpDegrees(a.yaw, b.yaw, t);
    aOut.pitch = a.pitch + (b.pitch - a.pitch) * t;
    aOut.rate = a.rate + (b.rate - a.rate) * t;
    // Discrete state switches at the midpoint so it doesn't lead or lag the motion.
    aOut.locomotion = t < 0.5f ? a.locomotion : b.locomotion;
    aOut.flags = t < 0.5f ? a.flags : b.flags;
    return Result::Interpolated;
}

AdaptiveDelay::AdaptiveDelay(TimeUs aSendIntervalUs, TimeUs aMinUs, TimeUs aMaxUs)
    : m_sendInterval(aSendIntervalUs)
    , m_min(aMinUs)
    , m_max(aMaxUs)
{
}

void AdaptiveDelay::OnArrival(TimeUs aSentAt, TimeUs aArrivedAt)
{
    const double transit = static_cast<double>(aArrivedAt - aSentAt);
    if (!m_hasTransit)
    {
        m_meanTransit = transit;
        m_hasTransit = true;
        return;
    }

    // RFC 3550-style smoothed jitter around a slowly adapting mean transit time.
    const double deviation = std::abs(transit - m_meanTransit);
    m_jitter += (deviation - m_jitter) / 16.0;
    m_meanTransit += (transit - m_meanTransit) / 64.0;
}

TimeUs AdaptiveDelay::DelayUs() const
{
    const double delay = 2.0 * static_cast<double>(m_sendInterval) + 2.0 * m_jitter;
    return Clamp(static_cast<TimeUs>(delay), m_min, m_max);
}
} // namespace coop
