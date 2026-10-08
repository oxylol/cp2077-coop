#pragma once

#include <map>
#include <vector>

#include "core/TimeField.hpp"
#include "core/Types.hpp"

namespace coop
{
// Every player's time rates, as any machine sees them (docs/01-architecture.md §8.2-8.5).
//
// Inputs, identical on every machine: all activations in the session (relayed by the host to everyone) and
// the host's grouping of players by proximity ("time field" membership). A player's world rate is the
// deepest slowdown among activations whose activator is in the same group:
//     r_p(T) = min(1, min over activations i with activator in group(p, T) of c_i(T))
// When a player's group changes (walked into or out of range, groups merged or split), their rate crossfades
// from the old group's rate to the new one over kRampUs. A player's own V runs at r_p / c_own (1 for the
// strongest activator), everyone else at r_p.
//
// Deterministic: machines with the same activations and groups compute the same rates for every player.
class TimeFieldClock
{
public:
    static constexpr TimeUs kRampUs = 300 * kUsPerMs;

    void Upsert(const TimeActivation& aActivation);
    bool SetEnd(uint32_t aId, TimeUs aEnd);

    // aPeer's group from aFrom on; aMembers includes aPeer. Ignored if it repeats the current group.
    void SetGroup(PeerId aPeer, TimeUs aFrom, std::vector<PeerId> aMembers);
    void RemovePeer(PeerId aPeer);

    // Global time dilation on aPeer's machine (the world around them).
    [[nodiscard]] float WorldRate(PeerId aPeer, TimeUs aTime) const;
    // aPeer's own V: 1 for the strongest activator, r / c for a weaker one, r for everyone else.
    [[nodiscard]] float PersonalRate(PeerId aPeer, TimeUs aTime) const;
    [[nodiscard]] bool IsActivating(PeerId aPeer, TimeUs aTime) const;

    // ∫ WorldRate(aPeer) over [aFrom, aTo] in microseconds: how much world time aPeer's machine simulates.
    [[nodiscard]] double IntegrateWorld(PeerId aPeer, TimeUs aFrom, TimeUs aTo) const;

    // Forgets activations that finished before aBefore and group history that no longer matters then.
    void Prune(TimeUs aBefore);

    [[nodiscard]] const std::map<uint32_t, TimeActivation>& Activations() const { return m_activations; }

private:
    struct Segment
    {
        TimeUs from = 0;
        std::vector<PeerId> members; // sorted, includes the peer
    };

    [[nodiscard]] double GroupRate(const std::vector<PeerId>& aMembers, TimeUs aTime) const;
    [[nodiscard]] double WorldRateValue(PeerId aPeer, TimeUs aTime) const;
    void CollectBreaks(PeerId aPeer, TimeUs aFrom, TimeUs aTo, std::vector<TimeUs>& aBreaks) const;
    [[nodiscard]] bool IsConstantOn(PeerId aPeer, TimeUs aFrom, TimeUs aTo) const;

    std::map<uint32_t, TimeActivation> m_activations;
    std::map<PeerId, std::vector<Segment>> m_groups;
};
} // namespace coop
