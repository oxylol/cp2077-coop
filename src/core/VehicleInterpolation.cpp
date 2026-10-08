#include "core/VehicleInterpolation.hpp"

#include <algorithm>

namespace coop
{
bool VehicleBuffer::Push(const VehicleSample& aSample)
{
    if (!m_samples.empty())
    {
        const TimeUs newest = m_samples.back().time;
        if (aSample.time == newest)
            return false;
        if (aSample.time < newest)
        {
            if (aSample.time < m_samples.front().time)
                return false;
            auto it = std::lower_bound(m_samples.begin(), m_samples.end(), aSample.time,
                                       [](const VehicleSample& aA, TimeUs aT) { return aA.time < aT; });
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

VehicleBuffer::Result VehicleBuffer::Sample(TimeUs aRenderTime, VehicleSample& aOut) const
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
        const TimeUs ahead = std::min(aRenderTime - newest.time, kMaxExtrapolationUs);
        aOut = newest;
        aOut.time = aRenderTime;
        aOut.position = newest.position + newest.velocity * static_cast<float>(ToSeconds(ahead));
        if (aRenderTime - newest.time <= kMaxExtrapolationUs)
            return Result::Extrapolated;
        aOut.velocity = {};
        return Result::Held;
    }

    auto upper = std::upper_bound(m_samples.begin(), m_samples.end(), aRenderTime,
                                  [](TimeUs aT, const VehicleSample& aS) { return aT < aS.time; });
    const auto& b = *upper;
    const auto& a = *(upper - 1);

    const double span = static_cast<double>(b.time - a.time);
    const float t = span > 0.0 ? static_cast<float>(static_cast<double>(aRenderTime - a.time) / span) : 0.0f;

    aOut.time = aRenderTime;
    aOut.position = Lerp(a.position, b.position, t);
    aOut.orientation = Nlerp(a.orientation, b.orientation, t);
    aOut.velocity = Lerp(a.velocity, b.velocity, t);
    aOut.steer = a.steer + (b.steer - a.steer) * t;
    aOut.throttle = a.throttle + (b.throttle - a.throttle) * t;
    aOut.brake = a.brake + (b.brake - a.brake) * t;
    aOut.flags = t < 0.5f ? a.flags : b.flags;
    return Result::Interpolated;
}
} // namespace coop
