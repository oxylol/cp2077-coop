#pragma once

struct NetworkService;

// Story co-op: the guests follow the host's time of day and weather. The host's game reports both every few seconds
// (ReportWorldState); the session passes them to the guests (NotifyWorldState), whose game moves its clock forward
// to the host's time of day and blends to the host's weather.
struct WorldSync
{
    // Host: sends the report when it's due. Call once per frame.
    void Report(NetworkService& aService);
    // Guest: follows the host's report.
    void Apply(uint32_t aGameTime, uint64_t aWeather);
    // Leaving the session: gives the weather back to the game.
    void Reset();

private:
    static constexpr auto kReportInterval = std::chrono::seconds(2);

    std::chrono::steady_clock::time_point m_nextReport{};
    Red::CName m_weather{}; // the host's weather this game was set to, if any
};
