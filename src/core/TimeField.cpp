#include "core/TimeField.hpp"

#include <algorithm>
#include <cmath>

#include "core/Math.hpp"

namespace coop
{
namespace
{
double Smooth(double aX)
{
    aX = Clamp(aX, 0.0, 1.0);
    return aX * aX * (3.0 - 2.0 * aX);
}

double DescentValue(const TimeActivation& aActivation, TimeUs aTime)
{
    if (aTime <= aActivation.start)
        return 1.0;
    if (aActivation.easeIn <= 0 || aTime >= aActivation.start + aActivation.easeIn)
        return aActivation.scale;
    const double x = static_cast<double>(aTime - aActivation.start) / static_cast<double>(aActivation.easeIn);
    return 1.0 + (static_cast<double>(aActivation.scale) - 1.0) * Smooth(x);
}

double CurveValue(const TimeActivation& aActivation, TimeUs aTime)
{
    if (aTime < aActivation.end)
        return DescentValue(aActivation, aTime);

    const double atEnd = DescentValue(aActivation, aActivation.end);
    if (aActivation.easeOut <= 0 || aTime >= aActivation.FinishedAt())
        return 1.0;
    const double x = static_cast<double>(aTime - aActivation.end) / static_cast<double>(aActivation.easeOut);
    return atEnd + (1.0 - atEnd) * Smooth(x);
}

double RateValue(const std::vector<TimeActivation>& aActivations, TimeUs aTime)
{
    double rate = 1.0;
    for (const auto& activation : aActivations)
        rate = std::min(rate, CurveValue(activation, aTime));
    return rate;
}

// Largest Simpson step inside eased segments. Small enough that the integration error is far below a
// microsecond for 300 ms smoothstep eases.
constexpr TimeUs kSimpsonStepUs = 1'000;
} // namespace

float ActivationCurve(const TimeActivation& aActivation, TimeUs aTime)
{
    return static_cast<float>(CurveValue(aActivation, aTime));
}

void FieldSchedule::SetAnchor(TimeUs aSessionTime, double aWorldTimeUs)
{
    m_anchorTime = aSessionTime;
    m_anchorWorld = aWorldTimeUs;
}

void FieldSchedule::Upsert(const TimeActivation& aActivation)
{
    auto it = std::lower_bound(m_activations.begin(), m_activations.end(), aActivation.id,
                               [](const TimeActivation& aA, uint32_t aId) { return aA.id < aId; });
    if (it != m_activations.end() && it->id == aActivation.id)
        *it = aActivation;
    else
        m_activations.insert(it, aActivation);
}

bool FieldSchedule::SetEnd(uint32_t aId, TimeUs aEnd)
{
    for (auto& activation : m_activations)
    {
        if (activation.id == aId)
        {
            activation.end = std::max(aEnd, activation.start);
            return true;
        }
    }
    return false;
}

float FieldSchedule::WorldRate(TimeUs aTime) const
{
    return static_cast<float>(RateValue(m_activations, aTime));
}

float FieldSchedule::PersonalRate(PeerId aPeer, TimeUs aTime) const
{
    const double world = RateValue(m_activations, aTime);

    double own = 1.0;
    bool isActivator = false;
    for (const auto& activation : m_activations)
    {
        if (activation.peer != aPeer)
            continue;
        const double value = CurveValue(activation, aTime);
        if (value < own)
        {
            own = value;
            isActivator = true;
        }
    }

    if (!isActivator || own <= 0.0)
        return static_cast<float>(world);
    return static_cast<float>(Clamp(world / own, 0.0, 1.0));
}

bool FieldSchedule::IsConstantOn(TimeUs aFrom, TimeUs aTo) const
{
    for (const auto& activation : m_activations)
    {
        const TimeUs easeInEnd = activation.start + activation.easeIn;
        const bool before = aTo <= activation.start;
        const bool holding = aFrom >= easeInEnd && aTo <= activation.end;
        const bool after = aFrom >= activation.FinishedAt();
        if (!before && !holding && !after)
            return false;
    }
    return true;
}

double FieldSchedule::Integrate(TimeUs aFrom, TimeUs aTo) const
{
    if (aTo <= aFrom)
        return 0.0;

    std::vector<TimeUs> breaks;
    breaks.reserve(2 + m_activations.size() * 4);
    breaks.push_back(aFrom);
    breaks.push_back(aTo);
    for (const auto& activation : m_activations)
    {
        for (TimeUs point : {activation.start, activation.start + activation.easeIn, activation.end,
                             activation.FinishedAt()})
        {
            if (point > aFrom && point < aTo)
                breaks.push_back(point);
        }
    }
    std::sort(breaks.begin(), breaks.end());
    breaks.erase(std::unique(breaks.begin(), breaks.end()), breaks.end());

    double total = 0.0;
    for (size_t i = 0; i + 1 < breaks.size(); ++i)
    {
        const TimeUs a = breaks[i];
        const TimeUs b = breaks[i + 1];
        if (IsConstantOn(a, b))
        {
            total += RateValue(m_activations, a + (b - a) / 2) * static_cast<double>(b - a);
            continue;
        }

        // Composite Simpson's rule with an even number of steps.
        TimeUs steps = std::max<TimeUs>(2, (b - a + kSimpsonStepUs - 1) / kSimpsonStepUs);
        if (steps % 2 != 0)
            ++steps;
        const double h = static_cast<double>(b - a) / static_cast<double>(steps);
        double sum = RateValue(m_activations, a) + RateValue(m_activations, b);
        for (TimeUs k = 1; k < steps; ++k)
        {
            const auto t = a + static_cast<TimeUs>(std::llround(h * static_cast<double>(k)));
            sum += RateValue(m_activations, t) * ((k % 2 == 1) ? 4.0 : 2.0);
        }
        total += sum * h / 3.0;
    }
    return total;
}

double FieldSchedule::WorldTimeAt(TimeUs aTime) const
{
    if (aTime <= m_anchorTime)
        return m_anchorWorld - static_cast<double>(m_anchorTime - aTime) * RateValue(m_activations, m_anchorTime);
    return m_anchorWorld + Integrate(m_anchorTime, aTime);
}

void FieldSchedule::Collect(TimeUs aNow)
{
    TimeUs latestFinish = m_anchorTime;
    bool anyFinished = false;
    for (const auto& activation : m_activations)
    {
        if (activation.FinishedAt() <= aNow && activation.FinishedAt() > m_anchorTime)
        {
            latestFinish = std::max(latestFinish, activation.FinishedAt());
            anyFinished = true;
        }
    }
    if (!anyFinished)
        return;

    // Only drop activations that are finished at the new anchor; keep everything else.
    const double worldAtFinish = WorldTimeAt(latestFinish);
    m_activations.erase(std::remove_if(m_activations.begin(), m_activations.end(),
                                       [latestFinish](const TimeActivation& aActivation)
                                       { return aActivation.FinishedAt() <= latestFinish; }),
                        m_activations.end());
    m_anchorTime = latestFinish;
    m_anchorWorld = worldAtFinish;
}

float ClockFollower::Compute(float aScheduledRate, double aTargetWorldUs, double aEngineWorldUs) const
{
    const double errorSeconds = (aTargetWorldUs - aEngineWorldUs) / static_cast<double>(kUsPerSecond);
    const double correction = Clamp(gainPerSecond * errorSeconds, -maxCorrection, maxCorrection);
    return static_cast<float>(std::max(0.01, static_cast<double>(aScheduledRate) + correction));
}

bool ClockFollower::NeedsResync(double aTargetWorldUs, double aEngineWorldUs) const
{
    return std::abs(aTargetWorldUs - aEngineWorldUs) > resyncThresholdUs;
}
} // namespace coop
