#pragma once

#include <array>
#include <cstdint>

namespace coop
{
// A player slot in a session. The host's own player is always peer 0.
using PeerId = uint8_t;
inline constexpr PeerId kHostPeer = 0;
inline constexpr PeerId kInvalidPeer = 0xFF;
inline constexpr int kMaxPlayers = 4;

// All times are integer microseconds.
// Session time (T) is real time synchronized to the host; it is never dilated.
// Field world time (W) runs slower inside a time field (docs/01-architecture.md §8).
using TimeUs = int64_t;
inline constexpr TimeUs kUsPerMs = 1'000;
inline constexpr TimeUs kUsPerSecond = 1'000'000;

inline constexpr double ToSeconds(TimeUs aUs)
{
    return static_cast<double>(aUs) / static_cast<double>(kUsPerSecond);
}

inline constexpr TimeUs FromSeconds(double aSeconds)
{
    return static_cast<TimeUs>(aSeconds * static_cast<double>(kUsPerSecond));
}

using Hash256 = std::array<uint8_t, 32>;
using Uuid = std::array<uint8_t, 16>;
} // namespace coop
