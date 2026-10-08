#include "client/SessionRunner.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

#include "core/Log.hpp"
#include "net/GnsTransport.hpp"

namespace coop
{
namespace
{
constexpr TimeUs kIdleTickUs = 50'000;
constexpr TimeUs kHostLingerUs = 500'000;
constexpr TimeUs kAppearanceIntervalUs = 1'000'000;
constexpr size_t kMaxQueuedStatus = 256;

// Every live runner, so plugin unload can stop their threads.
std::mutex g_runnersMutex;
std::set<SessionRunner*> g_runners;
} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// The IGameAdapter that ClientSession sees on the network thread. It never touches the game: it hands out
// the latest local sample published by the main thread and queues everything else for Pump().

class SessionRunner::NetSideAdapter final : public IGameAdapter
{
public:
    struct Event
    {
        enum class Kind
        {
            Joined,
            Appearance,
            Left,
            Status,
            VehicleSpawned,
            VehicleDespawned,
            VehicleAuthority,
            VehicleSeats,
        };
        Kind kind = Kind::Status;
        PeerId peer = kInvalidPeer;
        std::string text;
        LocalAppearance appearance;
        VehicleInfo vehicle; // netId is used by every vehicle event
        bool flag = false;   // authority: local
        std::vector<SeatAssignment> seats;
    };

    // --- network thread (IGameAdapter) ----------------------------------------------------------------------------

    bool CaptureLocal(LocalSample& aOut) override
    {
        std::scoped_lock lock(m_mutex);
        aOut = m_local;
        return true;
    }

    LocalAppearance GetLocalAppearance() override
    {
        std::scoped_lock lock(m_mutex);
        m_appearanceDirty = false;
        return m_appearance;
    }

    void OnRemotePlayerJoined(const RemotePlayerInfo& aInfo) override
    {
        std::scoped_lock lock(m_mutex);
        m_events.push_back({Event::Kind::Joined, aInfo.peer, aInfo.name, {}});
    }

    void OnRemoteAppearance(PeerId aPeer, const LocalAppearance& aAppearance) override
    {
        std::scoped_lock lock(m_mutex);
        m_events.push_back({Event::Kind::Appearance, aPeer, {}, aAppearance});
    }

    void OnRemotePlayerLeft(PeerId aPeer) override
    {
        std::scoped_lock lock(m_mutex);
        m_poses.erase(aPeer);
        m_events.push_back({Event::Kind::Left, aPeer, {}, {}});
    }

    void DriveRemotePlayer(PeerId aPeer, const RemotePose& aPose) override
    {
        std::scoped_lock lock(m_mutex);
        m_poses[aPeer] = aPose;
    }

    void OnStatus(const std::string& aText) override
    {
        std::scoped_lock lock(m_mutex);
        if (m_statusQueued < kMaxQueuedStatus)
        {
            m_events.push_back({Event::Kind::Status, kInvalidPeer, aText, {}});
            ++m_statusQueued;
        }
    }

    bool CaptureVehicle(uint32_t aNetId, VehicleSample& aOut) override
    {
        std::scoped_lock lock(m_mutex);
        auto it = m_localVehicles.find(aNetId);
        if (it == m_localVehicles.end())
            return false;
        aOut = it->second;
        return true;
    }

    void OnVehicleSpawned(const VehicleInfo& aInfo) override
    {
        std::scoped_lock lock(m_mutex);
        Event event;
        event.kind = Event::Kind::VehicleSpawned;
        event.vehicle = aInfo;
        m_events.push_back(std::move(event));
    }

    void OnVehicleDespawned(uint32_t aNetId) override
    {
        std::scoped_lock lock(m_mutex);
        m_vehiclePoses.erase(aNetId);
        m_localVehicles.erase(aNetId);
        Event event;
        event.kind = Event::Kind::VehicleDespawned;
        event.vehicle.netId = aNetId;
        m_events.push_back(std::move(event));
    }

    void OnVehicleAuthority(uint32_t aNetId, bool aLocal) override
    {
        std::scoped_lock lock(m_mutex);
        if (aLocal)
            m_vehiclePoses.erase(aNetId);
        else
            m_localVehicles.erase(aNetId); // stop sending the stale sample until the main thread republishes
        Event event;
        event.kind = Event::Kind::VehicleAuthority;
        event.vehicle.netId = aNetId;
        event.flag = aLocal;
        m_events.push_back(std::move(event));
    }

    void OnVehicleSeats(uint32_t aNetId, const std::vector<SeatAssignment>& aSeats) override
    {
        std::scoped_lock lock(m_mutex);
        Event event;
        event.kind = Event::Kind::VehicleSeats;
        event.vehicle.netId = aNetId;
        event.seats = aSeats;
        m_events.push_back(std::move(event));
    }

    void DriveVehicle(uint32_t aNetId, const VehiclePose& aPose) override
    {
        std::scoped_lock lock(m_mutex);
        m_vehiclePoses[aNetId] = aPose;
    }

    void ApplyTimeRates(const TimeRates& aRates) override
    {
        std::scoped_lock lock(m_mutex);
        m_timeRates = aRates;
        m_hasTimeRates = true;
    }

    // Main thread: the latest rates, if any arrived since the session started.
    bool LatestTimeRates(TimeRates& aOut)
    {
        std::scoped_lock lock(m_mutex);
        aOut = m_timeRates;
        return m_hasTimeRates;
    }

    // Main thread: the latest state of a vehicle this machine simulates.
    void PublishVehicle(uint32_t aNetId, const VehicleSample& aSample)
    {
        std::scoped_lock lock(m_mutex);
        m_localVehicles[aNetId] = aSample;
    }

    void DrainVehicles(std::vector<std::pair<uint32_t, VehiclePose>>& aPoses)
    {
        std::scoped_lock lock(m_mutex);
        aPoses.assign(m_vehiclePoses.begin(), m_vehiclePoses.end());
        m_vehiclePoses.clear();
    }

    // --- network thread helpers ------------------------------------------------------------------------------------

    bool TakeAppearanceChange(LocalAppearance& aOut)
    {
        std::scoped_lock lock(m_mutex);
        if (!m_appearanceDirty)
            return false;
        m_appearanceDirty = false;
        aOut = m_appearance;
        return true;
    }

    void NoteNetTick(TimeUs aNow)
    {
        std::scoped_lock lock(m_mutex);
        if (m_lastPublish >= 0)
            m_longestGap = std::max(m_longestGap, aNow - m_lastPublish);
    }

    // --- main thread -----------------------------------------------------------------------------------------------

    void PublishLocal(const LocalSample& aSample, TimeUs aNow)
    {
        std::scoped_lock lock(m_mutex);
        m_local = aSample;
        m_lastPublish = aNow;
    }

    void PublishAppearance(const LocalAppearance& aAppearance)
    {
        std::scoped_lock lock(m_mutex);
        if (aAppearance == m_appearance)
            return;
        m_appearance = aAppearance;
        m_appearanceDirty = true;
    }

    void Drain(std::vector<Event>& aEvents, std::vector<std::pair<PeerId, RemotePose>>& aPoses)
    {
        std::scoped_lock lock(m_mutex);
        aEvents.swap(m_events);
        m_events.clear();
        m_statusQueued = 0;
        aPoses.assign(m_poses.begin(), m_poses.end());
        m_poses.clear();
    }

    // Shared.

    void ResetSession()
    {
        std::scoped_lock lock(m_mutex);
        m_local = {};
        m_lastPublish = -1;
        m_longestGap = 0;
        m_localVehicles.clear();
        m_vehiclePoses.clear();
        m_timeRates = {};
        m_hasTimeRates = false;
    }

    TimeUs LongestGap() const
    {
        std::scoped_lock lock(m_mutex);
        return m_longestGap;
    }

private:
    mutable std::mutex m_mutex;
    LocalSample m_local;
    LocalAppearance m_appearance;
    bool m_appearanceDirty = false;
    TimeUs m_lastPublish = -1;
    TimeUs m_longestGap = 0;
    std::vector<Event> m_events;
    size_t m_statusQueued = 0;
    std::map<PeerId, RemotePose> m_poses;
    std::map<uint32_t, VehicleSample> m_localVehicles;
    std::map<uint32_t, VehiclePose> m_vehiclePoses;
    TimeRates m_timeRates;
    bool m_hasTimeRates = false;
};

// ---------------------------------------------------------------------------------------------------------------------

SessionRunner::SessionRunner(const IClock& aClock)
    : m_clock(aClock)
    , m_adapter(std::make_unique<NetSideAdapter>())
{
    std::scoped_lock lock(g_runnersMutex);
    g_runners.insert(this);
}

SessionRunner::~SessionRunner()
{
    {
        std::scoped_lock lock(g_runnersMutex);
        g_runners.erase(this);
    }
    StopThread();
    {
        std::scoped_lock lock(m_mutex);
        ShutdownLocked("shutting down");
        m_retiring.clear();
    }
    if (m_networkReady)
        GnsTransport::ShutdownLibrary(false);
}

void SessionRunner::StopAllThreads()
{
    std::vector<SessionRunner*> runners;
    {
        std::scoped_lock lock(g_runnersMutex);
        runners.assign(g_runners.begin(), g_runners.end());
    }
    for (auto* runner : runners)
        runner->StopThread();
}

bool SessionRunner::EnsureNetwork(std::string& aError)
{
    if (m_networkReady)
        return true;
    if (!GnsTransport::InitLibrary(aError))
        return false;
    m_networkReady = true;
    return true;
}

void SessionRunner::EnsureThread()
{
    if (m_thread.joinable())
        return;
    m_stopThread = false;
    m_thread = std::thread([this] { ThreadMain(); });
}

void SessionRunner::StopThread()
{
    {
        std::scoped_lock lock(m_mutex);
        if (!m_thread.joinable())
            return;
        m_stopThread = true;
    }
    m_wake.notify_all();
    m_thread.join();
}

void SessionRunner::ThreadMain()
{
    std::unique_lock lock(m_mutex);
    while (!m_stopThread)
    {
        TickLocked();
        const bool active = m_host || m_client;
        const auto wait = std::chrono::microseconds(active ? m_tickIntervalUs : kIdleTickUs);
        m_wake.wait_for(lock, wait, [this] { return m_stopThread; });
    }
}

void SessionRunner::TickLocked()
{
    ++m_netTicks;
    if (!m_retiring.empty())
    {
        const TimeUs now = m_clock.NowUs();
        std::erase_if(m_retiring, [&](const Retiring& aEntry) { return now >= aEntry.until; });
    }
    if (m_host)
        m_host->Tick();
    if (m_client)
    {
        LocalAppearance appearance;
        if (m_adapter->TakeAppearanceChange(appearance))
            m_client->UpdateAppearance(appearance);

        m_client->Tick();
        m_adapter->NoteNetTick(m_clock.NowUs());

        // A client-only session that ended (kicked, host gone, rejected) cleans itself up. A host keeps its own
        // player for as long as it hosts.
        if (m_client->State() == ClientState::Closed && !m_host)
        {
            m_client.reset();
            m_clientTransport.reset();
            m_activeFlag = false;
        }
    }
}

bool SessionRunner::Host(const HostConfig& aHost, const ClientConfig& aPlayer, std::string& aError)
{
    m_localVehicles.clear();
    std::scoped_lock lock(m_mutex);
    if (m_host || m_client)
    {
        aError = "already in a session; leave first";
        return false;
    }
    m_retiring.clear(); // hosting again right away needs the port back
    if (!EnsureNetwork(aError))
        return false;

    m_adapter->ResetSession();
    m_hostTransport = std::make_unique<GnsTransport>();
    m_host = std::make_unique<HostService>(*m_hostTransport, m_clock, aHost);
    if (!m_host->Start(aError))
    {
        m_host.reset();
        m_hostTransport.reset();
        return false;
    }

    m_clientTransport = std::make_unique<GnsTransport>();
    m_client = std::make_unique<ClientSession>(*m_clientTransport, *m_adapter, m_clock, aPlayer);

    ConnId hostSide = kInvalidConn;
    ConnId playerSide = kInvalidConn;
    if (!GnsTransport::CreatePair(*m_hostTransport, hostSide, *m_clientTransport, playerSide))
    {
        aError = "could not create the in-process connection";
        ShutdownLocked(aError);
        return false;
    }
    m_host->MarkLocal(hostSide);
    m_host->AdoptConnection(hostSide);
    m_client->UseConnection(playerSide);

    m_activeFlag = true;
    EnsureThread();
    return true;
}

bool SessionRunner::Join(const std::string& aAddress, const ClientConfig& aPlayer, std::string& aError)
{
    m_localVehicles.clear();
    std::scoped_lock lock(m_mutex);
    if (m_host || m_client)
    {
        aError = "already in a session; leave first";
        return false;
    }
    if (!EnsureNetwork(aError))
        return false;

    m_adapter->ResetSession();
    m_clientTransport = std::make_unique<GnsTransport>();
    m_client = std::make_unique<ClientSession>(*m_clientTransport, *m_adapter, m_clock, aPlayer);
    if (!m_client->Connect(aAddress, aError))
    {
        m_client.reset();
        m_clientTransport.reset();
        return false;
    }

    m_activeFlag = true;
    EnsureThread();
    return true;
}

void SessionRunner::Leave(const std::string& aReason)
{
    m_localVehicles.clear();
    std::scoped_lock lock(m_mutex);
    ShutdownLocked(aReason);
    // The network thread keeps idling (one wake-up every 50 ms) and finishes retiring the host transport.
}

void SessionRunner::ShutdownLocked(const std::string& aReason)
{
    if (m_client)
        m_client->Disconnect(aReason);
    if (m_host)
    {
        m_host->Stop();
        m_host->Tick(); // flush the kick messages
    }

    // Sessions close their connections through the transports, so they go first.
    m_client.reset();
    m_host.reset();
    m_clientTransport.reset();
    if (m_hostTransport)
        m_retiring.push_back({m_clock.NowUs() + kHostLingerUs, std::move(m_hostTransport)});
    m_activeFlag = false;
}

bool SessionRunner::SetImpairment(const std::string& aPreset)
{
    ImpairmentPreset preset;
    if (!ParseImpairmentPreset(aPreset, preset))
        return false;

    std::scoped_lock lock(m_mutex);
    std::string error;
    if (!EnsureNetwork(error))
        return false;
    GnsTransport::ApplyImpairment(preset);
    return true;
}

bool SessionRunner::IsActive() const
{
    std::scoped_lock lock(m_mutex);
    return m_host || m_client;
}

bool SessionRunner::IsHost() const
{
    std::scoped_lock lock(m_mutex);
    return m_host != nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Main thread

void SessionRunner::Pump(IGameAdapter& aGame)
{
    const TimeUs now = m_clock.NowUs();
    m_lastPump = now;

    // Outside a session only leftover events (players leaving, status) are delivered.
    if (m_activeFlag)
    {
        LocalSample sample;
        if (!aGame.CaptureLocal(sample))
            sample.valid = false;
        m_adapter->PublishLocal(sample, now);

        if (now >= m_nextAppearance)
        {
            m_nextAppearance = now + kAppearanceIntervalUs;
            m_adapter->PublishAppearance(aGame.GetLocalAppearance());
        }

        for (const auto netId : m_localVehicles)
        {
            VehicleSample vehicle;
            if (aGame.CaptureVehicle(netId, vehicle))
                m_adapter->PublishVehicle(netId, vehicle);
        }
    }

    std::vector<NetSideAdapter::Event> events;
    std::vector<std::pair<PeerId, RemotePose>> poses;
    m_adapter->Drain(events, poses);

    for (const auto& event : events)
    {
        switch (event.kind)
        {
        case NetSideAdapter::Event::Kind::Joined: aGame.OnRemotePlayerJoined({event.peer, event.text}); break;
        case NetSideAdapter::Event::Kind::Appearance: aGame.OnRemoteAppearance(event.peer, event.appearance); break;
        case NetSideAdapter::Event::Kind::Left: aGame.OnRemotePlayerLeft(event.peer); break;
        case NetSideAdapter::Event::Kind::Status:
            m_lastStatus = event.text;
            aGame.OnStatus(event.text);
            break;
        case NetSideAdapter::Event::Kind::VehicleSpawned: aGame.OnVehicleSpawned(event.vehicle); break;
        case NetSideAdapter::Event::Kind::VehicleDespawned:
            m_localVehicles.erase(event.vehicle.netId);
            aGame.OnVehicleDespawned(event.vehicle.netId);
            break;
        case NetSideAdapter::Event::Kind::VehicleAuthority:
            if (event.flag)
                m_localVehicles.insert(event.vehicle.netId);
            else
                m_localVehicles.erase(event.vehicle.netId);
            aGame.OnVehicleAuthority(event.vehicle.netId, event.flag);
            break;
        case NetSideAdapter::Event::Kind::VehicleSeats: aGame.OnVehicleSeats(event.vehicle.netId, event.seats); break;
        }
    }
    for (const auto& [peer, pose] : poses)
        aGame.DriveRemotePlayer(peer, pose);

    std::vector<std::pair<uint32_t, VehiclePose>> vehiclePoses;
    m_adapter->DrainVehicles(vehiclePoses);
    for (const auto& [netId, pose] : vehiclePoses)
        aGame.DriveVehicle(netId, pose);

    TimeRates rates;
    if (m_activeFlag && m_adapter->LatestTimeRates(rates))
        aGame.ApplyTimeRates(rates);
}

uint32_t SessionRunner::ActivateTimeField(msg::TimeFieldKind aKind, float aScale, TimeUs aDuration)
{
    std::scoped_lock lock(m_mutex);
    return m_client ? m_client->ActivateTimeField(aKind, aScale, aDuration) : 0;
}

void SessionRunner::CancelTimeField(uint32_t aId)
{
    std::scoped_lock lock(m_mutex);
    if (m_client)
        m_client->CancelTimeField(aId);
}

uint32_t SessionRunner::RegisterLocalVehicle(uint64_t aRecord, uint64_t aAppearance, const Vec3& aPosition,
                                             const Quat& aOrientation)
{
    uint32_t netId = 0;
    {
        std::scoped_lock lock(m_mutex);
        if (m_client)
            netId = m_client->RegisterLocalVehicle(aRecord, aAppearance, aPosition, aOrientation);
    }
    if (netId != 0)
    {
        m_localVehicles.insert(netId);
        VehicleSample initial;
        initial.position = aPosition;
        initial.orientation = aOrientation;
        m_adapter->PublishVehicle(netId, initial);
    }
    return netId;
}

void SessionRunner::DespawnLocalVehicle(uint32_t aNetId)
{
    std::scoped_lock lock(m_mutex);
    if (m_client)
        m_client->DespawnLocalVehicle(aNetId);
}

void SessionRunner::RequestSeat(uint32_t aNetId, uint8_t aSeat)
{
    std::scoped_lock lock(m_mutex);
    if (m_client)
        m_client->RequestSeat(aNetId, aSeat);
}

void SessionRunner::LeaveVehicle()
{
    std::scoped_lock lock(m_mutex);
    if (m_client)
        m_client->LeaveVehicle();
}

std::vector<ClientSession::VehicleView> SessionRunner::Vehicles() const
{
    std::scoped_lock lock(m_mutex);
    return m_client ? m_client->Vehicles() : std::vector<ClientSession::VehicleView>{};
}

PeerId SessionRunner::LocalPeer() const
{
    std::scoped_lock lock(m_mutex);
    return m_client ? m_client->LocalPeer() : kInvalidPeer;
}

SessionRunner::Status SessionRunner::GetStatus() const
{
    Status status;
    std::scoped_lock lock(m_mutex);
    status.active = m_host || m_client;
    status.hosting = m_host != nullptr;
    status.hostPlayers = m_host ? m_host->JoinedCount() : 0;
    if (m_client)
    {
        status.clientState = m_client->State();
        status.localPeer = m_client->LocalPeer();
        status.pingMs = m_client->Stats().pingMs;
        status.clockSynced = m_client->HasSessionTime();
        status.remotes = m_client->Remotes();
        status.timeRates = m_client->CurrentTimeRates();
    }
    status.netTicks = m_netTicks;
    status.longestMainThreadGapUs = m_adapter->LongestGap();
    return status;
}

std::string SessionRunner::StatusText() const
{
    const auto status = GetStatus();
    if (!status.active)
        return m_lastStatus.empty() ? "not in a session" : m_lastStatus;

    std::string text;
    char line[200];
    if (status.hosting)
        text += "hosting, " + std::to_string(status.hostPlayers) + " player(s)\n";
    if (status.clientState != ClientState::Idle)
    {
        std::snprintf(line, sizeof(line), "%s as player %u, ping %d ms, clock %s\n", ToString(status.clientState),
                      static_cast<unsigned>(status.localPeer), status.pingMs,
                      status.clockSynced ? "synced" : "syncing");
        text += line;
    }
    for (const auto& remote : status.remotes)
    {
        if (remote.hasPose)
            std::snprintf(line, sizeof(line), "  %s (player %u) at (%.1f, %.1f), delay %lld ms\n", remote.name.c_str(),
                          static_cast<unsigned>(remote.peer), remote.pose.position.x, remote.pose.position.y,
                          static_cast<long long>(remote.delayUs / kUsPerMs));
        else
            std::snprintf(line, sizeof(line), "  %s (player %u) no data yet\n", remote.name.c_str(),
                          static_cast<unsigned>(remote.peer));
        text += line;
    }
    if (status.timeRates.worldRate < 0.999f || status.timeRates.activating)
    {
        std::snprintf(line, sizeof(line), "  time field: world x%.2f, you x%.2f%s\n",
                      static_cast<double>(status.timeRates.worldRate), static_cast<double>(status.timeRates.localRate),
                      status.timeRates.activating ? " (activating)" : "");
        text += line;
    }
    if (status.longestMainThreadGapUs > 1'000'000)
    {
        std::snprintf(line, sizeof(line), "  longest game freeze kept alive: %.1f s\n",
                      static_cast<double>(status.longestMainThreadGapUs) / 1e6);
        text += line;
    }
    if (!m_lastStatus.empty())
        text += m_lastStatus + "\n";
    return text;
}
} // namespace coop
