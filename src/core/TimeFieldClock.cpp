#include "core/TimeFieldClock.hpp"

#include <algorithm>
#include <cmath>

#include "core/Math.hpp"

namespace coop
{
namespace
{
constexpr TimeUs kSimpsonStepUs = 2'000;

double Smooth(double aX)
{
    aX = Clamp(aX, 0.0, 1.0);
    return aX * aX * (3.0 - 2.0 * aX);
}

bool Contains(const std::vector<PeerId>& aSorted, PeerId aPeer)
{
    return std::binary_search(aSorted.begin(), aSorted.end(), aPeer);
}
} // namespace

void TimeFieldClock::Upsert(const TimeActivation& aActivation)
{
    m_activations[aActivation.id] = aActivation;
}

bool TimeFieldClock::SetEnd(uint32_t aId, TimeUs aEnd)
{
    auto it = m_activations.find(aId);
    if (it == m_activations.end())
        return false;
    it->second.end = std::max(aEnd, it->second.start);
    return true;
}

void TimeFieldClock::SetGroup(PeerId aPeer, TimeUs aFrom, std::vector<PeerId> aMembers)
{
    if (!Contains(aMembers, aPeer))
        aMembers.push_back(aPeer);
    std::sort(aMembers.begin(), aMembers.end());
    aMembers.erase(std::unique(aMembers.begin(), aMembers.end()), aMembers.end());

    auto& segments = m_groups[aPeer];
    if (!segments.empty() && segments.back().members == aMembers)
        return;
    // Segments stay in time order; an update older than what we have replaces the newer ones.
    while (!segments.empty() && segments.back().from >= aFrom)
        segments.pop_back();
    segments.push_back({aFrom, std::move(aMembers)});
}

void TimeFieldClock::RemovePeer(PeerId aPeer)
{
    m_groups.erase(aPeer);
}

double TimeFieldClock::GroupRate(const std::vector<PeerId>& aMembers, TimeUs aTime) const
{
    double rate = 1.0;
    for (const auto& [id, activation] : m_activations)
    {
        if (Contains(aMembers, activation.peer))
            rate = std::min(rate, static_cast<double>(ActivationCurve(activation, aTime)));
    }
    return rate;
}

double TimeFieldClock::WorldRateValue(PeerId aPeer, TimeUs aTime) const
{
    const std::vector<PeerId> alone{aPeer};
    auto it = m_groups.find(aPeer);
    if (it == m_groups.end() || it->second.empty() || aTime < it->second.front().from)
    {
        // No grouping known yet: only the player's own activations count. (Before the first segment the
        // crossfade into it hasn't started either.)
        return GroupRate(alone, aTime);
    }

    const auto& segments = it->second;
    auto current = std::upper_bound(segments.begin(), segments.end(), aTime,
                                    [](TimeUs aT, const Segment& aS) { return aT < aS.from; }) - 1;
    const double now = GroupRate(current->members, aTime);
    if (aTime >= current->from + kRampUs)
        return now;

    const auto& previous = current == segments.begin() ? alone : (current - 1)->members;
    const double before = GroupRate(previous, aTime);
    const double x = Smooth(static_cast<double>(aTime - current->from) / static_cast<double>(kRampUs));
    return before + (now - before) * x;
}

float TimeFieldClock::WorldRate(PeerId aPeer, TimeUs aTime) const
{
    return static_cast<float>(WorldRateValue(aPeer, aTime));
}

float TimeFieldClock::PersonalRate(PeerId aPeer, TimeUs aTime) const
{
    const double world = WorldRateValue(aPeer, aTime);
    double own = 1.0;
    for (const auto& [id, activation] : m_activations)
    {
        if (activation.peer == aPeer)
            own = std::min(own, static_cast<double>(ActivationCurve(activation, aTime)));
    }
    if (own >= 1.0 || own <= 0.0)
        return static_cast<float>(world);
    return static_cast<float>(Clamp(world / own, 0.0, 1.0));
}

bool TimeFieldClock::IsActivating(PeerId aPeer, TimeUs aTime) const
{
    for (const auto& [id, activation] : m_activations)
    {
        if (activation.peer == aPeer && aTime >= activation.start && aTime < activation.FinishedAt())
            return true;
    }
    return false;
}

void TimeFieldClock::CollectBreaks(PeerId aPeer, TimeUs aFrom, TimeUs aTo, std::vector<TimeUs>& aBreaks) const
{
    auto add = [&](TimeUs aPoint)
    {
        if (aPoint > aFrom && aPoint < aTo)
            aBreaks.push_back(aPoint);
    };
    for (const auto& [id, activation] : m_activations)
    {
        add(activation.start);
        add(activation.start + activation.easeIn);
        add(activation.end);
        add(activation.FinishedAt());
    }
    if (auto it = m_groups.find(aPeer); it != m_groups.end())
    {
        for (const auto& segment : it->second)
        {
            add(segment.from);
            add(segment.from + kRampUs);
        }
    }
}

bool TimeFieldClock::IsConstantOn(PeerId aPeer, TimeUs aFrom, TimeUs aTo) const
{
    // Inside [aFrom, aTo] (no break points inside): constant unless an ease or a group ramp overlaps it.
    for (const auto& [id, activation] : m_activations)
    {
        const bool easingIn = aFrom < activation.start + activation.easeIn && aTo > activation.start;
        const bool easingOut = aFrom < activation.FinishedAt() && aTo > activation.end;
        if (easingIn || easingOut)
            return false;
    }
    if (auto it = m_groups.find(aPeer); it != m_groups.end())
    {
        for (const auto& segment : it->second)
        {
            if (aFrom < segment.from + kRampUs && aTo > segment.from)
                return false;
        }
    }
    return true;
}

double TimeFieldClock::IntegrateWorld(PeerId aPeer, TimeUs aFrom, TimeUs aTo) const
{
    if (aTo <= aFrom)
        return 0.0;
    if (m_activations.empty())
        return static_cast<double>(aTo - aFrom);

    std::vector<TimeUs> breaks{aFrom, aTo};
    CollectBreaks(aPeer, aFrom, aTo, breaks);
    std::sort(breaks.begin(), breaks.end());
    breaks.erase(std::unique(breaks.begin(), breaks.end()), breaks.end());

    double total = 0.0;
    for (size_t i = 0; i + 1 < breaks.size(); ++i)
    {
        const TimeUs a = breaks[i];
        const TimeUs b = breaks[i + 1];
        if (IsConstantOn(aPeer, a, b))
        {
            total += WorldRateValue(aPeer, a + (b - a) / 2) * static_cast<double>(b - a);
            continue;
        }
        TimeUs steps = std::max<TimeUs>(2, (b - a + kSimpsonStepUs - 1) / kSimpsonStepUs);
        if (steps % 2 != 0)
            ++steps;
        const double h = static_cast<double>(b - a) / static_cast<double>(steps);
        double sum = WorldRateValue(aPeer, a) + WorldRateValue(aPeer, b);
        for (TimeUs k = 1; k < steps; ++k)
        {
            const auto t = a + static_cast<TimeUs>(std::llround(h * static_cast<double>(k)));
            sum += WorldRateValue(aPeer, t) * ((k % 2 == 1) ? 4.0 : 2.0);
        }
        total += sum * h / 3.0;
    }
    return total;
}

void TimeFieldClock::Prune(TimeUs aBefore)
{
    for (auto it = m_activations.begin(); it != m_activations.end();)
        it = it->second.FinishedAt() < aBefore ? m_activations.erase(it) : std::next(it);

    for (auto& [peer, segments] : m_groups)
    {
        // Keep the segment in force at aBefore (and the one before it while its ramp could still matter).
        size_t firstNeeded = 0;
        for (size_t i = 0; i < segments.size(); ++i)
        {
            if (segments[i].from + kRampUs <= aBefore)
                firstNeeded = i;
        }
        if (firstNeeded > 0)
            segments.erase(segments.begin(), segments.begin() + static_cast<long>(firstNeeded));
    }
}
} // namespace coop
