#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "client/GameAdapter.hpp"
#include "core/Clock.hpp"
#include "core/ClockSync.hpp"
#include "core/Interpolation.hpp"
#include "core/TimeFieldClock.hpp"
#include "core/VehicleInterpolation.hpp"
#include "core/Version.hpp"
#include "net/Transport.hpp"
#include "protocol/Messages.hpp"

namespace coop
{
struct ClientConfig
{
    std::string displayName = "V";
    std::string password;
    std::string modVersion = kVersionString;
    std::string gameBuild = "unknown";
    uint64_t exeSize = 0;
    Hash256 manifestHash{};
    Uuid clientId{};
    TimeUs connectTimeoutUs = 15 * kUsPerSecond;
};

enum class ClientState
{
    Idle,
    Connecting,
    Handshaking,
    Authenticating,
    Joined,
    Closed,
};

const char* ToString(ClientState aState);

// One player's side of a session (docs/01-architecture.md §3). Used by the game plugin for remote clients
// and for the host's own player (over an in-process connection), and by the sim tools for fake players.
class ClientSession
{
public:
    ClientSession(ITransport& aTransport, IGameAdapter& aAdapter, const IClock& aClock, ClientConfig aConfig);
    ~ClientSession();

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    bool Connect(const std::string& aAddress, std::string& aError);
    // Uses an existing connection (the host's own client over a loopback pair).
    void UseConnection(ConnId aConn);
    void Disconnect(const std::string& aReason);

    // Call once per frame. Polls the transport, handles messages, sends state, drives remote players.
    void Tick();

    // Sends the local appearance again if it changed since the last one sent (customization, clothing).
    void UpdateAppearance(const LocalAppearance& aAppearance);

    [[nodiscard]] ClientState State() const { return m_state; }
    [[nodiscard]] PeerId LocalPeer() const { return m_peer; }
    [[nodiscard]] const std::string& LastError() const { return m_lastError; }
    [[nodiscard]] bool HasSessionTime() const { return m_sync.HasEstimate(); }
    [[nodiscard]] TimeUs SessionNow() const { return m_sync.ToSession(m_clock.NowUs()); }
    [[nodiscard]] const ClockSync& Sync() const { return m_sync; }
    [[nodiscard]] ConnStats Stats() const;

    struct RemoteView
    {
        PeerId peer = kInvalidPeer;
        std::string name;
        uint16_t rttMs = 0;
        size_t bufferedSamples = 0;
        TimeUs delayUs = 0;
        bool hasPose = false;
        RemotePose pose;
        double worldOffsetUs = 0.0; // that player's world time minus session time (from their last state)
    };
    [[nodiscard]] std::vector<RemoteView> Remotes() const;

    [[nodiscard]] uint32_t StatesSent() const { return m_statesSent; }
    [[nodiscard]] uint32_t StatesReceived() const { return m_statesReceived; }

    // --- Animation inputs (core/AnimInput.hpp) ---

    // The local V's inputs since the last call: the latest value per input, events in order. Sent at up to 15 Hz
    // (what changed, plus events), and everything every 2 s.
    void PublishAnimInputs(const std::vector<AnimInput>& aInputs);
    [[nodiscard]] uint32_t AnimMessagesSent() const { return m_animSent; }
    [[nodiscard]] uint32_t AnimMessagesReceived() const { return m_animReceived; }

    // --- Vehicles (docs/02-systems.md §11) ---

    // Registers a vehicle this machine spawned (e.g. V summoned a car) and announces it. Returns its netId, or 0
    // when not joined or over the per-player limit.
    uint32_t RegisterLocalVehicle(uint64_t aRecord, uint64_t aAppearance, const Vec3& aPosition, const Quat& aOrientation);
    // Asks the host to remove one of this machine's vehicles; refused while another player sits in it.
    void DespawnLocalVehicle(uint32_t aNetId);
    // Asks the host for a seat (0 = driver). The answer arrives as OnVehicleSeats.
    void RequestSeat(uint32_t aNetId, uint8_t aSeat);
    void LeaveVehicle();
    // The local player's (vehicle, seat), if seated.
    [[nodiscard]] std::optional<std::pair<uint32_t, uint8_t>> LocalSeat() const;

    // --- Time fields: Sandevistan / Kerenzikov (docs/01-architecture.md §8) ---

    // Starts slow motion now: applied locally at once, then announced. Returns the activation id (0 if not joined).
    uint32_t ActivateTimeField(msg::TimeFieldKind aKind, float aScale, TimeUs aDuration,
                               TimeUs aEaseIn = 300 * kUsPerMs, TimeUs aEaseOut = 300 * kUsPerMs);
    // Ends one of this player's activations now (with the normal ease-out).
    void CancelTimeField(uint32_t aId);

    [[nodiscard]] const TimeFieldClock& TimeClock() const { return m_timeClock; }
    [[nodiscard]] TimeRates CurrentTimeRates() const { return m_timeRates; }
    // World time this machine should have simulated by session time aTime (W = T outside time fields).
    [[nodiscard]] double WorldTimeAt(TimeUs aTime) const;
    [[nodiscard]] uint16_t TimeGroup() const { return m_timeGroup; }

    struct VehicleView
    {
        uint32_t netId = 0;
        uint64_t record = 0;
        PeerId spawner = kInvalidPeer;
        PeerId owner = kInvalidPeer;
        uint16_t epoch = 0;
        bool local = false; // this machine simulates it
        std::vector<SeatAssignment> seats;
        bool hasPose = false;
        VehiclePose pose;
    };
    [[nodiscard]] std::vector<VehicleView> Vehicles() const;

private:
    struct Remote
    {
        std::string name;
        uint16_t rttMs = 0;
        InterpolationBuffer buffer;
        AdaptiveDelay delay;
        bool hasPose = false;
        RemotePose lastPose;
        double worldOffsetUs = 0.0;
    };

    void HandleEvent(const ConnEvent& aEvent);
    void HandlePacket(const Packet& aPacket);
    void OnJoinAccept(const msg::JoinAccept& aMessage);
    void AddRemote(PeerId aPeer, const std::string& aName);
    void RemoveRemote(PeerId aPeer);
    void Fail(const std::string& aReason);

    struct Vehicle
    {
        VehicleInfo info;
        PeerId owner = kInvalidPeer;
        uint16_t epoch = 0;
        bool local = false;
        std::vector<SeatAssignment> seats;
        VehicleBuffer buffer;
        AdaptiveDelay delay;
        uint32_t seq = 0;
        bool hasPose = false;
        VehiclePose lastPose;
    };

    void SendTimeSync(TimeUs aLocalNow);
    void SendLocalState(TimeUs aLocalNow);
    void SendAnimInputs(TimeUs aLocalNow);
    void DriveRemotes();

    void HandleVehicleSpawn(const msg::VehicleSpawn& aSpawn);
    void HandleVehicleDespawn(const msg::VehicleDespawn& aDespawn);
    void HandleSeatState(const msg::VehicleSeatState& aState);
    void HandleVehicleState(const msg::VehicleState& aState);
    void SendVehicleStates(TimeUs aLocalNow);
    void DriveVehicles(TimeUs aSessionNow);
    void ClearVehicles();

    void HandleTimeFieldActivate(const msg::TimeFieldActivate& aActivate);
    void HandleMembership(const msg::TimeFieldMembership& aMembership);
    void UpdateTimeRates(TimeUs aSessionNow);
    [[nodiscard]] TimeUs StateSendInterval() const;

    template<typename M>
    bool SendMessage(const M& aMessage);

    ITransport& m_transport;
    IGameAdapter& m_adapter;
    const IClock& m_clock;
    ClientConfig m_config;

    ClientState m_state = ClientState::Idle;
    ConnId m_conn = kInvalidConn;
    PeerId m_peer = kInvalidPeer;
    std::string m_lastError;
    TimeUs m_connectStarted = 0;

    ClockSync m_sync;
    TimeUs m_nextPing = 0;
    int m_burstPingsLeft = 0;
    TimeUs m_nextHeartbeat = 0;
    uint32_t m_heartbeatSeq = 0;

    TimeUs m_stateInterval = 33'333;
    TimeUs m_nextStateSend = 0;
    uint32_t m_stateSeq = 0;
    bool m_hasPrevious = false;
    Vec3 m_previousPosition;
    TimeUs m_previousTime = 0;

    LocalAppearance m_sentAppearance;
    bool m_appearanceSent = false;

    uint32_t m_statesSent = 0;
    uint32_t m_statesReceived = 0;

    std::map<PeerId, Remote> m_remotes;

    std::map<uint32_t, Vehicle> m_vehicles;

    TimeFieldClock m_timeClock;
    TimeRates m_timeRates;
    uint16_t m_timeGroup = 0;
    uint32_t m_nextActivation = 1;
    // World time: W(T) = anchorW + ∫ rate from anchorT. The anchor trails a second behind, so activations and
    // group changes that arrive late still correct recent world time (docs/01 §8.4).
    bool m_worldClockStarted = false;
    TimeUs m_worldAnchorT = 0;
    double m_worldAnchorW = 0.0;
    TimeUs m_nextTimePrune = 0;
    uint32_t m_nextVehicle = 1;
    TimeUs m_nextVehicleSend = 0;

    std::map<uint64_t, AnimInput> m_animCurrent; // latest value of each local input, by AnimInput::Key()
    std::set<uint64_t> m_animDirty;
    std::vector<AnimInput> m_animEvents;
    TimeUs m_nextAnimSend = 0;
    TimeUs m_nextAnimFull = 0;
    uint32_t m_animSent = 0;
    uint32_t m_animReceived = 0;
};
} // namespace coop
