#pragma once

#include <cstdint>
#include <vector>

#include "core/Types.hpp"

namespace coop
{
// Shared slow motion (Sandevistan / Kerenzikov). See docs/01-architecture.md §8.
//
// Each activation defines a curve c(T) over session time T: 1 before it starts, eased down to `scale`,
// held, then eased back to 1. A field's world rate is r(T) = min(1, min_i c_i(T)). Because min is
// order-independent, every machine that knows the same activations computes the same r(T) and the same
// world time W(T) = W(T0) + ∫ r dT, regardless of message arrival order.
struct TimeActivation
{
    uint32_t id = 0;     // unique per session (high byte: activator peer, low bits: counter)
    PeerId peer = kInvalidPeer;
    float scale = 1.0f;  // 0 < scale <= 1, e.g. 0.25 for a strong Sandevistan
    TimeUs start = 0;    // session time
    TimeUs end = 0;      // session time when the ease-out begins (start + duration, or an early cancel)
    TimeUs easeIn = 300 * kUsPerMs;
    TimeUs easeOut = 300 * kUsPerMs;

    [[nodiscard]] TimeUs FinishedAt() const { return end + easeOut; }
};

// Smoothstep curve for one activation.
float ActivationCurve(const TimeActivation& aActivation, TimeUs aTime);

class FieldSchedule
{
public:
    // The point from which world time is integrated. All members of a field must share it.
    void SetAnchor(TimeUs aSessionTime, double aWorldTimeUs);

    // Adds an activation or replaces the one with the same id (e.g. an early cancel moves `end`).
    void Upsert(const TimeActivation& aActivation);
    bool SetEnd(uint32_t aId, TimeUs aEnd);

    [[nodiscard]] float WorldRate(TimeUs aTime) const;

    // Real-time rate of a player inside the field: r / c for an activator (1 for the strongest),
    // r for everyone else.
    [[nodiscard]] float PersonalRate(PeerId aPeer, TimeUs aTime) const;

    // Field world time at session time aTime (aTime >= anchor). Returned in microseconds.
    [[nodiscard]] double WorldTimeAt(TimeUs aTime) const;

    // Drops activations whose ease-out has fully finished at or before aNow, re-anchoring at the latest
    // such finish time. The anchor moves only to schedule-defined instants, so every machine computes the
    // same anchor no matter when it calls this.
    void Collect(TimeUs aNow);

    [[nodiscard]] bool HasActivations() const { return !m_activations.empty(); }
    [[nodiscard]] const std::vector<TimeActivation>& Activations() const { return m_activations; }
    [[nodiscard]] TimeUs AnchorTime() const { return m_anchorTime; }
    [[nodiscard]] double AnchorWorldTime() const { return m_anchorWorld; }

private:
    [[nodiscard]] double Integrate(TimeUs aFrom, TimeUs aTo) const;
    [[nodiscard]] bool IsConstantOn(TimeUs aFrom, TimeUs aTo) const;

    std::vector<TimeActivation> m_activations; // sorted by id
    TimeUs m_anchorTime = 0;
    double m_anchorWorld = 0.0;
};

// Keeps a machine's engine world time on the shared schedule. Returns the time dilation to apply this
// frame: the scheduled rate plus a correction proportional to the error (additive, so a slowed world
// converges as fast as a normal one), clamped to ±maxCorrection.
struct ClockFollower
{
    double gainPerSecond = 8.0;   // 10 ms of error -> +0.08 rate; error decays with a ~125 ms time constant
    double maxCorrection = 0.15;  // absolute rate units
    double resyncThresholdUs = 250'000.0;

    [[nodiscard]] float Compute(float aScheduledRate, double aTargetWorldUs, double aEngineWorldUs) const;
    [[nodiscard]] bool NeedsResync(double aTargetWorldUs, double aEngineWorldUs) const;
};
} // namespace coop
