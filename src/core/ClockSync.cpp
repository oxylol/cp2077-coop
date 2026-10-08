#include "core/ClockSync.hpp"

#include <algorithm>
#include <vector>

#include "core/Math.hpp"

namespace coop
{
void ClockSync::AddSample(TimeUs aLocalSend, TimeUs aHostTime, TimeUs aLocalRecv)
{
    const TimeUs rtt = aLocalRecv - aLocalSend;
    if (rtt < 0)
        return; // clock went backwards or a bogus sample

    Sample sample;
    sample.rtt = rtt;
    sample.offset = aHostTime + rtt / 2 - aLocalRecv;

    m_samples[m_next] = sample;
    m_next = (m_next + 1) % kWindow;
    m_count = std::min(m_count + 1, kWindow);

    std::vector<Sample> window(m_samples.begin(), m_samples.begin() + static_cast<std::ptrdiff_t>(m_count));
    std::sort(window.begin(), window.end(), [](const Sample& aA, const Sample& aB) { return aA.rtt < aB.rtt; });

    const size_t useCount = std::max<size_t>(1, (window.size() + 1) / 2);
    std::vector<TimeUs> offsets;
    offsets.reserve(useCount);
    for (size_t i = 0; i < useCount; ++i)
        offsets.push_back(window[i].offset);
    std::sort(offsets.begin(), offsets.end());

    const TimeUs target = offsets[offsets.size() / 2];
    m_bestRtt = window.front().rtt;

    if (!m_hasEstimate)
    {
        m_offset = target;
        m_hasEstimate = true;
        return;
    }

    const TimeUs error = target - m_offset;
    if (error > kSnapThresholdUs || error < -kSnapThresholdUs)
        m_offset = target;
    else
        m_offset += Clamp(error, -kMaxSlewUs, kMaxSlewUs);
}

void ClockSync::Reset()
{
    *this = ClockSync{};
}
} // namespace coop
