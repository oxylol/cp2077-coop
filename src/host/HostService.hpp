#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/Clock.hpp"
#include "core/Version.hpp"
#include "net/Transport.hpp"
#include "protocol/Messages.hpp"

namespace coop
{
struct HostConfig
{
    uint16_t port = kDefaultPort;
    std::string hostName = "Host";
    std::string password;
    std::string modVersion = kVersionString;
    std::string gameBuild = "unknown";
    uint64_t exeSize = 0;
    bool requireSameBuild = true;
    // Development: accept SimClient tools (game build "sim") even when requireSameBuild is on.
    bool allowSimClients = false;
    int maxPlayers = kMaxPlayers;
    uint32_t passwordIterations = 100'000;
    uint16_t stateRateHz = 30;
    TimeUs peerTimeoutUs = 15 * kUsPerSecond;
    TimeUs handshakeTimeoutUs = 15 * kUsPerSecond;

    // Time fields (docs/01-architecture.md §8.5): players closer than this share a field, transitively.
    // [VERIFY] against the interest/streaming range once cells exist (M1); 150 m covers a firefight.
    float timeFieldRadius = 150.0f;
    bool globalTimeFields = false; // host setting: every player is always in one field
    TimeUs groupIntervalUs = 500 * kUsPerMs;
};

// The host's arbiter and relay (docs/01-architecture.md §3.2). M0 scope: handshake, password, version
// checks, roster, player-state relay, clock-sync responder, timeouts, vehicle seats and ownership. Has no
// engine dependencies, so it runs identically inside the game and in the standalone sim host.
class HostService
{
public:
    HostService(ITransport& aTransport, const IClock& aClock, HostConfig aConfig);
    ~HostService();

    HostService(const HostService&) = delete;
    HostService& operator=(const HostService&) = delete;

    // aListen=false runs without a listen socket (in-process tests).
    bool Start(std::string& aError, bool aListen = true);
    void Stop();
    [[nodiscard]] bool Running() const { return m_running; }

    // Marks a connection as the host's own player; it receives peer id 0 when it joins.
    void MarkLocal(ConnId aConn);
    // Adopts a connection created outside the transport's listen socket (in-process pair).
    void AdoptConnection(ConnId aConn);

    void Tick();

    [[nodiscard]] TimeUs SessionNow() const { return m_clock.NowUs() - m_startTime; }
    [[nodiscard]] size_t JoinedCount() const;
    [[nodiscard]] std::vector<msg::RosterEntry> Roster() const;
    [[nodiscard]] const Uuid& WorldId() const { return m_worldId; }
    [[nodiscard]] const HostConfig& Config() const { return m_config; }

    struct Counters
    {
        uint64_t statesRelayed = 0;
        uint64_t animRelayed = 0;
        uint64_t rejected = 0;
        uint64_t malformed = 0;
        uint64_t vehicleStatesRelayed = 0;
        uint64_t vehicleStatesDropped = 0; // from a machine that doesn't own the vehicle, or an old epoch
        uint64_t vehicleRequestsDenied = 0;
        uint64_t timeFieldsRefused = 0;
    };
    [[nodiscard]] const Counters& Stats() const { return m_counters; }

    struct VehicleView
    {
        uint32_t netId = 0;
        PeerId spawner = kInvalidPeer;
        PeerId owner = kInvalidPeer;
        uint16_t epoch = 0;
        std::vector<msg::SeatEntry> seats; // sorted by seat
    };
    [[nodiscard]] std::vector<VehicleView> Vehicles() const;

    // Time fields: the current proximity grouping and the activations the host knows about.
    [[nodiscard]] std::vector<msg::MembershipEntry> TimeGroups() const;
    [[nodiscard]] std::vector<msg::TimeFieldActivate> TimeActivations() const;

private:
    enum class PeerState
    {
        AwaitHello,
        AwaitAuth,
        Joined,
    };

    struct Peer
    {
        ConnId conn = kInvalidConn;
        PeerId id = kInvalidPeer;
        PeerState state = PeerState::AwaitHello;
        bool local = false;
        std::string name;
        Uuid clientId{};
        std::array<uint8_t, 32> nonce{};
        TimeUs connectedAt = 0;
        TimeUs lastHeard = 0;
        uint16_t rttMs = 0;
        std::optional<msg::PlayerAppearance> appearance;
    };

    struct Vehicle
    {
        msg::VehicleSpawn spawn;
        PeerId owner = kInvalidPeer;
        uint16_t epoch = 1;
        std::map<uint8_t, PeerId> seats;
    };

    void OnConnected(ConnId aConn);
    void OnDisconnected(ConnId aConn, const std::string& aReason);
    void HandlePacket(Peer& aPeer, const Packet& aPacket);
    void HandleHello(Peer& aPeer, const msg::Hello& aHello);
    void HandleAuth(Peer& aPeer, const msg::AuthResponse& aResponse);
    void Join(Peer& aPeer);
    void RemovePeer(ConnId aConn, RejectReason aReason, const std::string& aDetail, bool aNotifyPeer);
    void BroadcastRoster();

    void HandleVehicleSpawn(Peer& aPeer, const msg::VehicleSpawn& aSpawn);
    void HandleVehicleDespawn(Peer& aPeer, const msg::VehicleDespawn& aDespawn);
    void HandleSeatRequest(Peer& aPeer, const msg::VehicleSeatRequest& aRequest);
    void RelayVehicleState(Peer& aPeer, const msg::VehicleState& aState);
    void DespawnVehicle(uint32_t aNetId, msg::VehicleDespawn::Reason aReason);
    void OnPeerLeftVehicles(PeerId aPeer);

    void HandleTimeFieldActivate(Peer& aPeer, msg::TimeFieldActivate aActivate);
    void HandleTimeFieldDeactivate(Peer& aPeer, const msg::TimeFieldDeactivate& aDeactivate);
    void OnPeerLeftTimeFields(PeerId aPeer);
    void UpdateTimeGroups(bool aForceBroadcast);
    [[nodiscard]] msg::TimeFieldMembership MakeMembership() const;
    [[nodiscard]] msg::VehicleSeatState MakeSeatState(uint32_t aNetId, const Vehicle& aVehicle) const;

    PeerId AllocatePeerId(bool aLocal) const;
    Peer* FindJoined(PeerId aPeer);

    template<typename M>
    void SendTo(const Peer& aPeer, const M& aMessage);
    template<typename M>
    void Broadcast(const M& aMessage, ConnId aExcept = kInvalidConn);
    void SendRawToJoined(Lane aLane, const std::vector<uint8_t>& aBytes, bool aReliable, ConnId aExcept);

    ITransport& m_transport;
    const IClock& m_clock;
    HostConfig m_config;

    bool m_running = false;
    TimeUs m_startTime = 0;
    TimeUs m_nextRoster = 0;
    Uuid m_worldId{};
    std::array<uint8_t, 16> m_salt{};
    std::vector<uint8_t> m_passwordKey;

    std::map<ConnId, Peer> m_peers;
    std::map<uint32_t, Vehicle> m_vehicles;

    struct TimeGroup
    {
        uint16_t id = 0;
        std::vector<PeerId> members; // sorted
        TimeUs changedAt = 0;
    };
    std::map<uint32_t, msg::TimeFieldActivate> m_timeActivations;
    std::map<PeerId, Vec3> m_positions; // latest reported player positions
    std::map<PeerId, TimeGroup> m_timeGroups;
    uint16_t m_nextTimeGroup = 1;
    TimeUs m_nextGroupUpdate = 0;
    ConnId m_localConn = kInvalidConn;
    Counters m_counters;
};
} // namespace coop
