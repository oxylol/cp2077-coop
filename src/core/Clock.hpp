#pragma once

#include <atomic>
#include <chrono>

#include "core/Types.hpp"

namespace coop
{
// Monotonic local clock. Injected everywhere so tests can drive time deterministically.
class IClock
{
public:
    virtual ~IClock() = default;
    [[nodiscard]] virtual TimeUs NowUs() const = 0;
};

class SteadyClock final : public IClock
{
public:
    SteadyClock()
        : m_origin(std::chrono::steady_clock::now())
    {
    }

    [[nodiscard]] TimeUs NowUs() const override
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_origin)
            .count();
    }

private:
    std::chrono::steady_clock::time_point m_origin;
};

// Clocks are read from the network thread and the main thread, so this one is atomic too.
class ManualClock final : public IClock
{
public:
    [[nodiscard]] TimeUs NowUs() const override { return m_now.load(); }
    void Set(TimeUs aNow) { m_now.store(aNow); }
    void Advance(TimeUs aDelta) { m_now.fetch_add(aDelta); }

private:
    std::atomic<TimeUs> m_now{0};
};
} // namespace coop
