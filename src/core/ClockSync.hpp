#pragma once

#include <array>
#include <cstddef>

#include "core/Types.hpp"

namespace coop
{
// Estimates the offset between a client's local clock and the host's session clock from
// ping/pong exchanges (NTP-style, filtered by minimum round-trip time).
//
//   localSend  -- client local time when the ping was sent
//   hostTime   -- host session time when the ping was answered
//   localRecv  -- client local time when the pong arrived
//
// offset ≈ hostTime + rtt/2 - localRecv. Samples with the lowest RTT carry the least queuing error,
// so the estimate uses the median offset among the fastest half of a sliding window.
// After the first estimate, changes are slewed (at most kMaxSlewUs per sample) unless the error is
// large, in which case the estimate snaps.
class ClockSync
{
public:
    static constexpr size_t kWindow = 16;
    static constexpr TimeUs kSnapThresholdUs = 50 * kUsPerMs;
    static constexpr TimeUs kMaxSlewUs = 2 * kUsPerMs;

    void AddSample(TimeUs aLocalSend, TimeUs aHostTime, TimeUs aLocalRecv);
    void Reset();

    [[nodiscard]] bool HasEstimate() const { return m_hasEstimate; }
    [[nodiscard]] TimeUs OffsetUs() const { return m_offset; }
    [[nodiscard]] TimeUs RttUs() const { return m_bestRtt; }
    [[nodiscard]] size_t SampleCount() const { return m_count; }

    // Converts a local clock reading to host session time.
    [[nodiscard]] TimeUs ToSession(TimeUs aLocal) const { return aLocal + m_offset; }

private:
    struct Sample
    {
        TimeUs offset = 0;
        TimeUs rtt = 0;
    };

    std::array<Sample, kWindow> m_samples{};
    size_t m_next = 0;
    size_t m_count = 0;
    bool m_hasEstimate = false;
    TimeUs m_offset = 0;
    TimeUs m_bestRtt = 0;
};
} // namespace coop
