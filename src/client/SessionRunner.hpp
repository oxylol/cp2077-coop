#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "client/ClientSession.hpp"
#include "client/GameAdapter.hpp"
#include "core/Clock.hpp"
#include "host/HostService.hpp"

namespace coop
{
class GnsTransport;

// Runs one machine's side of a session on a dedicated network thread (docs/01-architecture.md §10).
//
// The network thread owns the transports, the HostService (when hosting) and this machine's ClientSession.
// It keeps heartbeats, clock sync and relaying going even while the main thread is blocked by a loading
// screen. The game is touched only from the main thread: Pump() hands over the latest local sample and
// delivers queued events and remote poses to the game's IGameAdapter.
//
// Locking: one session mutex covers everything the network thread ticks; main-thread calls (Host, Join,
// Leave, status) take it briefly. The queue mutex inside the adapter is only ever taken after the session
// mutex or on its own, never the other way round.
class SessionRunner
{
public:
    explicit SessionRunner(const IClock& aClock);
    ~SessionRunner();

    SessionRunner(const SessionRunner&) = delete;
    SessionRunner& operator=(const SessionRunner&) = delete;

    // Hosting: a HostService plus this machine's player over an in-process connection.
    bool Host(const HostConfig& aHost, const ClientConfig& aPlayer, std::string& aError);
    bool Join(const std::string& aAddress, const ClientConfig& aPlayer, std::string& aError);
    void Leave(const std::string& aReason);

    // Network impairment for testing (process-wide); initializes the network library if needed.
    bool SetImpairment(const std::string& aPreset);

    [[nodiscard]] bool IsActive() const;
    [[nodiscard]] bool IsHost() const;

    // Main thread, once per frame. All calls into aGame happen here.
    void Pump(IGameAdapter& aGame);

    // Vehicles (main thread; see ClientSession). RegisterLocalVehicle returns the netId, 0 if not joined.
    uint32_t RegisterLocalVehicle(uint64_t aRecord, uint64_t aAppearance, const Vec3& aPosition, const Quat& aOrientation);
    void DespawnLocalVehicle(uint32_t aNetId);
    void RequestSeat(uint32_t aNetId, uint8_t aSeat);
    void LeaveVehicle();
    [[nodiscard]] std::vector<ClientSession::VehicleView> Vehicles() const;
    [[nodiscard]] PeerId LocalPeer() const;

    // Time fields (main thread; see ClientSession). Returns the activation id, 0 if not joined.
    uint32_t ActivateTimeField(msg::TimeFieldKind aKind, float aScale, TimeUs aDuration);
    void CancelTimeField(uint32_t aId);

    struct Status
    {
        bool active = false;
        bool hosting = false;
        size_t hostPlayers = 0;
        ClientState clientState = ClientState::Idle;
        PeerId localPeer = kInvalidPeer;
        int pingMs = -1;
        bool clockSynced = false;
        std::vector<ClientSession::RemoteView> remotes;
        uint64_t netTicks = 0;
        TimeUs longestMainThreadGapUs = 0; // longest time without a Pump(), e.g. a loading screen
        TimeRates timeRates;
    };
    [[nodiscard]] Status GetStatus() const;
    // Multi-line text for the dev panel.
    [[nodiscard]] std::string StatusText() const;
    [[nodiscard]] const std::string& LastStatus() const { return m_lastStatus; }

    // Network thread tick interval (default 4 ms).
    void SetTickInterval(TimeUs aInterval) { m_tickIntervalUs = aInterval; }

    // Stops every runner's network thread; for plugin unload.
    static void StopAllThreads();

private:
    class NetSideAdapter;

    bool EnsureNetwork(std::string& aError);
    void EnsureThread();
    void StopThread();
    void ThreadMain();
    void TickLocked();
    void ShutdownLocked(const std::string& aReason);

    const IClock& m_clock;
    std::unique_ptr<NetSideAdapter> m_adapter;

    mutable std::mutex m_mutex; // the session mutex
    std::condition_variable m_wake;
    std::thread m_thread;
    bool m_stopThread = false;
    TimeUs m_tickIntervalUs = 4'000;
    uint64_t m_netTicks = 0;

    std::atomic<bool> m_activeFlag{false}; // mirrors (m_host || m_client) for lock-free checks in Pump()
    bool m_networkReady = false;
    std::unique_ptr<GnsTransport> m_hostTransport;
    std::unique_ptr<GnsTransport> m_clientTransport;
    std::unique_ptr<HostService> m_host;
    std::unique_ptr<ClientSession> m_client;

    // A host's transport outlives the session briefly: destroying its listen socket would cut off the final
    // "session closed" messages still being sent to clients.
    struct Retiring
    {
        TimeUs until = 0;
        std::unique_ptr<GnsTransport> transport;
    };
    std::vector<Retiring> m_retiring;

    // Main thread only.
    std::set<uint32_t> m_localVehicles; // vehicles this machine simulates, as last told by the network thread
    std::string m_lastStatus;
    TimeUs m_lastPump = -1;
    TimeUs m_nextAppearance = 0;
};
} // namespace coop
