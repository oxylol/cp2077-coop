#include "host/HostService.hpp"

#include <algorithm>
#include <cstdlib>
#include <set>

#include "core/Crypto.hpp"
#include "core/Log.hpp"
#include "protocol/Auth.hpp"
#include "protocol/Codec.hpp"

namespace coop
{
namespace
{
constexpr TimeUs kRosterIntervalUs = 1 * kUsPerSecond;
constexpr const char* kSimBuild = "sim";
} // namespace

HostService::HostService(ITransport& aTransport, const IClock& aClock, HostConfig aConfig)
    : m_transport(aTransport)
    , m_clock(aClock)
    , m_config(std::move(aConfig))
{
    m_config.maxPlayers = std::clamp(m_config.maxPlayers, 1, kMaxPlayers);
}

HostService::~HostService()
{
    Stop();
}

bool HostService::Start(std::string& aError, bool aListen)
{
    if (m_running)
        return true;

    if (aListen && !m_transport.Listen(m_config.port, aError))
        return false;

    crypto::RandomBytes(m_worldId.data(), m_worldId.size());
    crypto::RandomBytes(m_salt.data(), m_salt.size());
    if (!m_config.password.empty())
        m_passwordKey = auth::DeriveKey(m_config.password, m_salt, m_config.passwordIterations);

    m_startTime = m_clock.NowUs();
    m_nextRoster = 0;
    m_running = true;
    COOP_LOG_INFO("host: session started%s%s", aListen ? " on UDP port " : "",
                  aListen ? std::to_string(m_config.port).c_str() : "");
    return true;
}

void HostService::Stop()
{
    if (!m_running)
        return;

    const auto peers = m_peers;
    for (const auto& [conn, peer] : peers)
        RemovePeer(conn, RejectReason::HostShutdown, {}, true);
    m_peers.clear();
    m_vehicles.clear();
    m_timeActivations.clear();
    m_positions.clear();
    m_timeGroups.clear();
    m_running = false;
    COOP_LOG_INFO("host: session stopped");
}

void HostService::MarkLocal(ConnId aConn)
{
    m_localConn = aConn;
    auto it = m_peers.find(aConn);
    if (it != m_peers.end())
        it->second.local = true;
}

void HostService::AdoptConnection(ConnId aConn)
{
    OnConnected(aConn);
}

size_t HostService::JoinedCount() const
{
    return static_cast<size_t>(std::count_if(m_peers.begin(), m_peers.end(),
                                             [](const auto& aPair) { return aPair.second.state == PeerState::Joined; }));
}

std::vector<msg::RosterEntry> HostService::Roster() const
{
    std::vector<msg::RosterEntry> roster;
    for (const auto& [conn, peer] : m_peers)
    {
        if (peer.state != PeerState::Joined)
            continue;
        msg::RosterEntry entry;
        entry.peer = peer.id;
        entry.name = peer.name;
        entry.state = 1;
        entry.rttMs = peer.rttMs;
        roster.push_back(entry);
    }
    std::sort(roster.begin(), roster.end(), [](const auto& aA, const auto& aB) { return aA.peer < aB.peer; });
    return roster;
}

// ---------------------------------------------------------------------------------------------------------------------
// Sending

template<typename M>
void HostService::SendTo(const Peer& aPeer, const M& aMessage)
{
    const auto bytes = Encode(aMessage);
    if (bytes.empty())
    {
        COOP_LOG_ERROR("host: failed to encode %s", ToString(M::kId));
        return;
    }
    m_transport.Send(aPeer.conn, M::kLane, bytes, M::kReliable);
}

template<typename M>
void HostService::Broadcast(const M& aMessage, ConnId aExcept)
{
    const auto bytes = Encode(aMessage);
    if (bytes.empty())
    {
        COOP_LOG_ERROR("host: failed to encode %s", ToString(M::kId));
        return;
    }
    SendRawToJoined(M::kLane, bytes, M::kReliable, aExcept);
}

void HostService::SendRawToJoined(Lane aLane, const std::vector<uint8_t>& aBytes, bool aReliable, ConnId aExcept)
{
    for (const auto& [conn, peer] : m_peers)
    {
        if (peer.state == PeerState::Joined && conn != aExcept)
            m_transport.Send(conn, aLane, aBytes, aReliable);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Tick

void HostService::Tick()
{
    if (!m_running)
        return;

    std::vector<ConnEvent> events;
    std::vector<Packet> packets;
    m_transport.Poll(events, packets);

    for (const auto& event : events)
    {
        switch (event.type)
        {
        case ConnEventType::Incoming:
        case ConnEventType::Connected: OnConnected(event.conn); break;
        case ConnEventType::Disconnected: OnDisconnected(event.conn, event.reason); break;
        }
    }

    const TimeUs now = m_clock.NowUs();
    for (const auto& packet : packets)
    {
        auto it = m_peers.find(packet.conn);
        if (it == m_peers.end())
            continue;
        it->second.lastHeard = now;
        HandlePacket(it->second, packet);
    }

    // Timeouts.
    std::vector<ConnId> expired;
    for (const auto& [conn, peer] : m_peers)
    {
        if (peer.state != PeerState::Joined && now - peer.connectedAt > m_config.handshakeTimeoutUs)
            expired.push_back(conn);
        else if (peer.state == PeerState::Joined && now - peer.lastHeard > m_config.peerTimeoutUs)
            expired.push_back(conn);
    }
    for (const auto conn : expired)
        RemovePeer(conn, RejectReason::Timeout, {}, true);

    if (now >= m_nextGroupUpdate)
    {
        m_nextGroupUpdate = now + m_config.groupIntervalUs;
        UpdateTimeGroups(false);
    }

    if (now >= m_nextRoster)
    {
        m_nextRoster = now + kRosterIntervalUs;
        for (auto& [conn, peer] : m_peers)
        {
            const auto stats = m_transport.Stats(conn);
            if (stats.pingMs >= 0)
                peer.rttMs = static_cast<uint16_t>(std::min(stats.pingMs, 65535));
        }
        BroadcastRoster();
    }
}

void HostService::OnConnected(ConnId aConn)
{
    if (m_peers.count(aConn))
        return;

    Peer peer;
    peer.conn = aConn;
    peer.local = aConn == m_localConn;
    peer.connectedAt = m_clock.NowUs();
    peer.lastHeard = peer.connectedAt;
    m_peers.emplace(aConn, peer);
    COOP_LOG_DEBUG("host: connection %u opened", aConn);
}

void HostService::OnDisconnected(ConnId aConn, const std::string& aReason)
{
    auto it = m_peers.find(aConn);
    if (it == m_peers.end())
        return;
    COOP_LOG_INFO("host: connection %u closed (%s)", aConn, aReason.c_str());
    RemovePeer(aConn, RejectReason::None, aReason, false);
}

void HostService::RemovePeer(ConnId aConn, RejectReason aReason, const std::string& aDetail, bool aNotifyPeer)
{
    auto it = m_peers.find(aConn);
    if (it == m_peers.end())
        return;

    const Peer peer = it->second;
    if (aNotifyPeer)
    {
        if (peer.state == PeerState::Joined)
            SendTo(peer, msg::Kick{aReason, aDetail});
        else
            SendTo(peer, msg::Reject{aReason, aDetail});
        m_transport.Close(aConn, static_cast<int>(aReason), ToString(aReason));
    }
    m_peers.erase(it);
    if (aConn == m_localConn)
        m_localConn = kInvalidConn;

    if (peer.state == PeerState::Joined)
    {
        COOP_LOG_INFO("host: %s (peer %u) left", peer.name.c_str(), static_cast<unsigned>(peer.id));
        Broadcast(msg::PlayerLeft{peer.id, aReason});
        OnPeerLeftVehicles(peer.id);
        OnPeerLeftTimeFields(peer.id);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Vehicles (docs/02-systems.md §11)

msg::VehicleSeatState HostService::MakeSeatState(uint32_t aNetId, const Vehicle& aVehicle) const
{
    msg::VehicleSeatState state;
    state.netId = aNetId;
    state.owner = aVehicle.owner;
    state.epoch = aVehicle.epoch;
    for (const auto& [seat, peer] : aVehicle.seats)
        state.seats.push_back({seat, peer});
    return state;
}

std::vector<HostService::VehicleView> HostService::Vehicles() const
{
    std::vector<VehicleView> views;
    for (const auto& [netId, vehicle] : m_vehicles)
    {
        VehicleView view;
        view.netId = netId;
        view.spawner = msg::VehicleSpawner(netId);
        view.owner = vehicle.owner;
        view.epoch = vehicle.epoch;
        view.seats = MakeSeatState(netId, vehicle).seats;
        views.push_back(std::move(view));
    }
    return views;
}

void HostService::HandleVehicleSpawn(Peer& aPeer, const msg::VehicleSpawn& aSpawn)
{
    // Each machine allocates ids in its own range, so two machines can never pick the same one.
    const bool validId = msg::VehicleSpawner(aSpawn.netId) == aPeer.id && (aSpawn.netId & 0x00FFFFFFu) != 0;
    const auto spawnedByPeer = std::count_if(m_vehicles.begin(), m_vehicles.end(), [&](const auto& aEntry)
                                             { return msg::VehicleSpawner(aEntry.first) == aPeer.id; });
    if (!validId || m_vehicles.count(aSpawn.netId) || spawnedByPeer >= static_cast<long>(msg::kMaxVehiclesPerPeer))
    {
        ++m_counters.vehicleRequestsDenied;
        COOP_LOG_WARN("host: refused vehicle %08x from peer %u", aSpawn.netId, static_cast<unsigned>(aPeer.id));
        return;
    }

    Vehicle vehicle;
    vehicle.spawn = aSpawn;
    vehicle.owner = aPeer.id;
    vehicle.epoch = 1;
    const auto& stored = m_vehicles.emplace(aSpawn.netId, std::move(vehicle)).first->second;

    Broadcast(aSpawn, aPeer.conn); // the others spawn proxies
    Broadcast(MakeSeatState(aSpawn.netId, stored));
}

void HostService::HandleVehicleDespawn(Peer& aPeer, const msg::VehicleDespawn& aDespawn)
{
    auto it = m_vehicles.find(aDespawn.netId);
    if (it == m_vehicles.end())
        return;

    // Only the spawner can send its vehicle away, and not while somebody else is sitting in it.
    const bool othersInside = std::any_of(it->second.seats.begin(), it->second.seats.end(),
                                          [&](const auto& aSeat) { return aSeat.second != aPeer.id; });
    if (msg::VehicleSpawner(aDespawn.netId) != aPeer.id || othersInside)
    {
        ++m_counters.vehicleRequestsDenied;
        SendTo(aPeer, MakeSeatState(it->first, it->second)); // resync the requester
        return;
    }
    DespawnVehicle(aDespawn.netId, msg::VehicleDespawn::Reason::Dismissed);
}

void HostService::HandleSeatRequest(Peer& aPeer, const msg::VehicleSeatRequest& aRequest)
{
    auto it = m_vehicles.find(aRequest.netId);
    if (it == m_vehicles.end())
    {
        ++m_counters.vehicleRequestsDenied;
        return;
    }
    Vehicle& vehicle = it->second;

    if (aRequest.seat == msg::kLeaveSeat)
    {
        if (std::erase_if(vehicle.seats, [&](const auto& aSeat) { return aSeat.second == aPeer.id; }) > 0)
            Broadcast(MakeSeatState(it->first, vehicle));
        return;
    }

    const auto occupant = vehicle.seats.find(aRequest.seat);
    const bool taken = occupant != vehicle.seats.end() && occupant->second != aPeer.id;
    if (aRequest.seat >= msg::kMaxVehicleSeats || taken)
    {
        ++m_counters.vehicleRequestsDenied;
        SendTo(aPeer, MakeSeatState(it->first, vehicle)); // the requester sees it didn't get the seat
        return;
    }

    // A player sits in one seat at a time: leave any other seat first, in this vehicle or another.
    for (auto& [netId, other] : m_vehicles)
    {
        const auto removed = std::erase_if(other.seats, [&](const auto& aSeat) { return aSeat.second == aPeer.id; });
        if (removed > 0 && netId != aRequest.netId)
            Broadcast(MakeSeatState(netId, other));
    }

    vehicle.seats[aRequest.seat] = aPeer.id;
    // The driver's machine simulates the vehicle (docs/02-systems.md §11).
    if (aRequest.seat == msg::kDriverSeat && vehicle.owner != aPeer.id)
    {
        vehicle.owner = aPeer.id;
        ++vehicle.epoch;
    }
    Broadcast(MakeSeatState(it->first, vehicle));
}

void HostService::RelayVehicleState(Peer& aPeer, const msg::VehicleState& aState)
{
    auto it = m_vehicles.find(aState.netId);
    if (it == m_vehicles.end() || it->second.owner != aPeer.id || it->second.epoch != aState.epoch)
    {
        // Normal for a moment after a handoff: the previous driver's last snapshots are still in flight.
        ++m_counters.vehicleStatesDropped;
        return;
    }
    const auto bytes = Encode(aState, kFlagRelayed);
    SendRawToJoined(msg::VehicleState::kLane, bytes, false, aPeer.conn);
    ++m_counters.vehicleStatesRelayed;
}

void HostService::DespawnVehicle(uint32_t aNetId, msg::VehicleDespawn::Reason aReason)
{
    if (m_vehicles.erase(aNetId) > 0)
        Broadcast(msg::VehicleDespawn{aNetId, aReason});
}

void HostService::OnPeerLeftVehicles(PeerId aPeer)
{
    // A player's own vehicles leave with them.
    std::vector<uint32_t> owned;
    for (const auto& [netId, vehicle] : m_vehicles)
    {
        if (msg::VehicleSpawner(netId) == aPeer)
            owned.push_back(netId);
    }
    for (const auto netId : owned)
        DespawnVehicle(netId, msg::VehicleDespawn::Reason::SpawnerLeft);

    // Seats they held free up; a vehicle they were driving goes back to its spawner's machine.
    for (auto& [netId, vehicle] : m_vehicles)
    {
        bool changed = std::erase_if(vehicle.seats, [&](const auto& aSeat) { return aSeat.second == aPeer; }) > 0;
        if (vehicle.owner == aPeer)
        {
            vehicle.owner = msg::VehicleSpawner(netId);
            ++vehicle.epoch;
            changed = true;
        }
        if (changed)
            Broadcast(MakeSeatState(netId, vehicle));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Messages

void HostService::HandlePacket(Peer& aPeer, const Packet& aPacket)
{
    EnvelopeHeader header;
    const uint8_t* data = aPacket.data.data();
    const size_t size = aPacket.data.size();
    if (!PeekHeader(data, size, header))
    {
        ++m_counters.malformed;
        return;
    }

    // Clock sync is answered in any state so clients can start estimating early.
    if (header.id == MsgId::TimeSyncPing)
    {
        msg::TimeSyncPing ping;
        if (Decode(data, size, ping))
            SendTo(aPeer, msg::TimeSyncPong{ping.clientSendUs, SessionNow()});
        return;
    }

    switch (aPeer.state)
    {
    case PeerState::AwaitHello:
    {
        msg::Hello hello;
        if (header.id == MsgId::Hello && Decode(data, size, hello))
            HandleHello(aPeer, hello);
        else
            ++m_counters.malformed;
        return;
    }
    case PeerState::AwaitAuth:
    {
        msg::AuthResponse response;
        if (header.id == MsgId::AuthResponse && Decode(data, size, response))
            HandleAuth(aPeer, response);
        else
            ++m_counters.malformed;
        return;
    }
    case PeerState::Joined: break;
    }

    switch (header.id)
    {
    case MsgId::PlayerState:
    {
        msg::PlayerState state;
        if (!Decode(data, size, state))
        {
            ++m_counters.malformed;
            return;
        }
        state.peer = aPeer.id; // never trust the sender's claim
        m_positions[aPeer.id] = state.position;
        const auto bytes = Encode(state, kFlagRelayed);
        SendRawToJoined(msg::PlayerState::kLane, bytes, false, aPeer.conn);
        ++m_counters.statesRelayed;
        break;
    }

    case MsgId::TimeFieldActivate:
    {
        msg::TimeFieldActivate activate;
        if (!Decode(data, size, activate))
        {
            ++m_counters.malformed;
            return;
        }
        HandleTimeFieldActivate(aPeer, activate);
        break;
    }

    case MsgId::TimeFieldDeactivate:
    {
        msg::TimeFieldDeactivate deactivate;
        if (!Decode(data, size, deactivate))
        {
            ++m_counters.malformed;
            return;
        }
        HandleTimeFieldDeactivate(aPeer, deactivate);
        break;
    }

    case MsgId::VehicleState:
    {
        msg::VehicleState state;
        if (!Decode(data, size, state))
        {
            ++m_counters.malformed;
            return;
        }
        RelayVehicleState(aPeer, state);
        break;
    }

    case MsgId::VehicleSpawn:
    {
        msg::VehicleSpawn spawn;
        if (!Decode(data, size, spawn))
        {
            ++m_counters.malformed;
            return;
        }
        HandleVehicleSpawn(aPeer, spawn);
        break;
    }

    case MsgId::VehicleDespawn:
    {
        msg::VehicleDespawn despawn;
        if (!Decode(data, size, despawn))
        {
            ++m_counters.malformed;
            return;
        }
        HandleVehicleDespawn(aPeer, despawn);
        break;
    }

    case MsgId::VehicleSeatRequest:
    {
        msg::VehicleSeatRequest request;
        if (!Decode(data, size, request))
        {
            ++m_counters.malformed;
            return;
        }
        HandleSeatRequest(aPeer, request);
        break;
    }

    case MsgId::PlayerAppearance:
    {
        msg::PlayerAppearance appearance;
        if (!Decode(data, size, appearance))
        {
            ++m_counters.malformed;
            return;
        }
        appearance.peer = aPeer.id;
        aPeer.appearance = appearance;
        Broadcast(appearance, aPeer.conn);
        break;
    }

    case MsgId::Chat:
    {
        msg::Chat chat;
        if (!Decode(data, size, chat))
            return;
        chat.peer = aPeer.id;
        Broadcast(chat, aPeer.conn);
        break;
    }

    case MsgId::Heartbeat:
    case MsgId::ClientReady:
        break;

    default:
        COOP_LOG_DEBUG("host: ignoring %s from peer %u", ToString(header.id), static_cast<unsigned>(aPeer.id));
        break;
    }
}

void HostService::HandleHello(Peer& aPeer, const msg::Hello& aHello)
{
    aPeer.name = aHello.displayName.empty() ? "V" : aHello.displayName;
    aPeer.clientId = aHello.clientId;

    auto reject = [&](RejectReason aReason, const std::string& aDetail)
    {
        COOP_LOG_INFO("host: rejecting %s: %s %s", aPeer.name.c_str(), ToString(aReason), aDetail.c_str());
        ++m_counters.rejected;
        RemovePeer(aPeer.conn, aReason, aDetail, true);
    };

    if (aHello.protocolVersion != kProtocolVersion)
    {
        reject(RejectReason::ProtocolMismatch, "host " + std::to_string(kProtocolVersion) + ", you "
                                                   + std::to_string(aHello.protocolVersion));
        return;
    }
    if (aHello.modVersion != m_config.modVersion)
    {
        reject(RejectReason::ModVersionMismatch, "host " + m_config.modVersion + ", you " + aHello.modVersion);
        return;
    }

    const bool isSim = aHello.gameBuild == kSimBuild;
    if (m_config.requireSameBuild && !(isSim && m_config.allowSimClients)
        && (aHello.gameBuild != m_config.gameBuild || aHello.exeSize != m_config.exeSize))
    {
        reject(RejectReason::GameBuildMismatch, "host " + m_config.gameBuild + ", you " + aHello.gameBuild);
        return;
    }

    if (!aPeer.local && AllocatePeerId(false) == kInvalidPeer)
    {
        reject(RejectReason::SessionFull, {});
        return;
    }

    msg::Challenge challenge;
    challenge.salt = m_salt;
    crypto::RandomBytes(aPeer.nonce.data(), aPeer.nonce.size());
    challenge.nonce = aPeer.nonce;
    // The host's own player never needs the password.
    challenge.passwordRequired = !m_passwordKey.empty() && !aPeer.local;
    challenge.iterations = m_config.passwordIterations;
    SendTo(aPeer, challenge);
    aPeer.state = PeerState::AwaitAuth;
}

void HostService::HandleAuth(Peer& aPeer, const msg::AuthResponse& aResponse)
{
    if (!m_passwordKey.empty() && !aPeer.local)
    {
        const auto expected = auth::Proof(m_passwordKey, aPeer.nonce, aPeer.clientId);
        if (!crypto::Equal(expected.data(), aResponse.proof.data(), expected.size()))
        {
            COOP_LOG_INFO("host: wrong password from %s", aPeer.name.c_str());
            ++m_counters.rejected;
            RemovePeer(aPeer.conn, RejectReason::BadPassword, {}, true);
            return;
        }
    }
    Join(aPeer);
}

void HostService::Join(Peer& aPeer)
{
    const PeerId id = AllocatePeerId(aPeer.local);
    if (id == kInvalidPeer)
    {
        RemovePeer(aPeer.conn, RejectReason::SessionFull, {}, true);
        return;
    }

    aPeer.id = id;
    aPeer.state = PeerState::Joined;

    msg::JoinAccept accept;
    accept.peer = id;
    accept.worldId = m_worldId;
    accept.stateRateHz = m_config.stateRateHz;
    accept.roster = Roster();
    SendTo(aPeer, accept);

    // Late joiners need everyone's appearance and the vehicles that exist.
    for (const auto& [conn, other] : m_peers)
    {
        if (conn != aPeer.conn && other.state == PeerState::Joined && other.appearance)
            SendTo(aPeer, *other.appearance);
    }
    for (const auto& [netId, vehicle] : m_vehicles)
    {
        SendTo(aPeer, vehicle.spawn);
        SendTo(aPeer, MakeSeatState(netId, vehicle));
    }
    for (const auto& [id, activation] : m_timeActivations)
        SendTo(aPeer, activation);
    UpdateTimeGroups(true); // includes the newcomer (alone until its first position arrives)

    Broadcast(msg::PlayerJoined{id, aPeer.name}, aPeer.conn);
    COOP_LOG_INFO("host: %s joined as peer %u%s", aPeer.name.c_str(), static_cast<unsigned>(id),
                  aPeer.local ? " (host player)" : "");
}

PeerId HostService::AllocatePeerId(bool aLocal) const
{
    auto taken = [this](PeerId aId)
    {
        return std::any_of(m_peers.begin(), m_peers.end(), [aId](const auto& aPair)
                           { return aPair.second.state == PeerState::Joined && aPair.second.id == aId; });
    };

    if (aLocal)
        return taken(kHostPeer) ? kInvalidPeer : kHostPeer;

    for (int id = 1; id < m_config.maxPlayers; ++id)
    {
        if (!taken(static_cast<PeerId>(id)))
            return static_cast<PeerId>(id);
    }
    return kInvalidPeer;
}

HostService::Peer* HostService::FindJoined(PeerId aPeer)
{
    for (auto& [conn, peer] : m_peers)
    {
        if (peer.state == PeerState::Joined && peer.id == aPeer)
            return &peer;
    }
    return nullptr;
}

void HostService::BroadcastRoster()
{
    msg::RosterState roster;
    roster.entries = Roster();
    Broadcast(roster);
}
// ---------------------------------------------------------------------------------------------------------------------
// Time fields (docs/01-architecture.md §8)

void HostService::HandleTimeFieldActivate(Peer& aPeer, msg::TimeFieldActivate aActivate)
{
    const TimeUs now = SessionNow();
    const bool validId = (aActivate.activationId >> 24) == aPeer.id && (aActivate.activationId & 0x00FFFFFFu) != 0;
    const bool validScale = aActivate.scaleUnits >= msg::kMinTimeScaleUnits && aActivate.scaleUnits <= msg::kTimeScaleUnits;
    const bool validTimes = aActivate.end > aActivate.start
                         && aActivate.end - aActivate.start <= msg::kMaxTimeFieldDurationUs
                         && std::llabs(aActivate.start - now) <= 2 * kUsPerSecond;
    const bool validEases = aActivate.easeInMs <= msg::kMaxTimeFieldEaseMs && aActivate.easeOutMs <= msg::kMaxTimeFieldEaseMs;
    if (!validId || !validScale || !validTimes || !validEases || m_timeActivations.count(aActivate.activationId))
    {
        ++m_counters.timeFieldsRefused;
        COOP_LOG_WARN("host: refused time field %08x from peer %u", aActivate.activationId,
                      static_cast<unsigned>(aPeer.id));
        // The activator already applied it locally; end it right away so its machine eases back out.
        msg::TimeFieldDeactivate cancel{aActivate.activationId, aActivate.start};
        SendTo(aPeer, cancel);
        return;
    }

    aActivate.peer = aPeer.id;
    m_timeActivations[aActivate.activationId] = aActivate;
    Broadcast(aActivate, aPeer.conn);
}

void HostService::HandleTimeFieldDeactivate(Peer& aPeer, const msg::TimeFieldDeactivate& aDeactivate)
{
    auto it = m_timeActivations.find(aDeactivate.activationId);
    if (it == m_timeActivations.end() || it->second.peer != aPeer.id)
    {
        ++m_counters.timeFieldsRefused;
        return;
    }
    // Ending early only: an activation can't be extended after the fact.
    const TimeUs end = std::clamp(aDeactivate.end, it->second.start, it->second.end);
    it->second.end = end;
    Broadcast(msg::TimeFieldDeactivate{aDeactivate.activationId, end}, aPeer.conn);
}

void HostService::OnPeerLeftTimeFields(PeerId aPeer)
{
    // A disconnected activator's slow-motion ends at the disconnect, with the normal ease-out.
    const TimeUs now = SessionNow();
    for (auto& [id, activation] : m_timeActivations)
    {
        if (activation.peer == aPeer && activation.end > now)
        {
            activation.end = std::max(now, activation.start);
            Broadcast(msg::TimeFieldDeactivate{id, activation.end});
        }
    }
    m_positions.erase(aPeer);
    m_timeGroups.erase(aPeer);
    UpdateTimeGroups(true);
}

std::vector<msg::MembershipEntry> HostService::TimeGroups() const
{
    return MakeMembership().entries;
}

std::vector<msg::TimeFieldActivate> HostService::TimeActivations() const
{
    std::vector<msg::TimeFieldActivate> activations;
    for (const auto& [id, activation] : m_timeActivations)
        activations.push_back(activation);
    return activations;
}

msg::TimeFieldMembership HostService::MakeMembership() const
{
    msg::TimeFieldMembership membership;
    membership.global = m_config.globalTimeFields;
    for (const auto& [peer, group] : m_timeGroups)
        membership.entries.push_back({peer, group.id, group.changedAt});
    return membership;
}

void HostService::UpdateTimeGroups(bool aForceBroadcast)
{
    const TimeUs now = SessionNow();

    // Forget finished activations after a grace period (late joiners don't need them).
    for (auto it = m_timeActivations.begin(); it != m_timeActivations.end();)
    {
        const TimeUs finished = it->second.end + static_cast<TimeUs>(it->second.easeOutMs) * kUsPerMs;
        it = finished + 10 * kUsPerSecond < now ? m_timeActivations.erase(it) : std::next(it);
    }

    std::vector<PeerId> peers;
    for (const auto& [conn, peer] : m_peers)
    {
        if (peer.state == PeerState::Joined)
            peers.push_back(peer.id);
    }
    std::sort(peers.begin(), peers.end());

    // Connected components: players within the radius of each other, transitively (union-find).
    std::map<PeerId, PeerId> parent;
    for (const auto peer : peers)
        parent[peer] = peer;
    auto find = [&](PeerId aPeer)
    {
        while (parent[aPeer] != aPeer)
            aPeer = parent[aPeer] = parent[parent[aPeer]];
        return aPeer;
    };
    for (size_t i = 0; i < peers.size(); ++i)
    {
        for (size_t j = i + 1; j < peers.size(); ++j)
        {
            bool linked = m_config.globalTimeFields;
            const auto a = m_positions.find(peers[i]);
            const auto b = m_positions.find(peers[j]);
            if (!linked && a != m_positions.end() && b != m_positions.end())
                linked = Distance(a->second, b->second) <= m_config.timeFieldRadius;
            if (linked)
                parent[find(peers[i])] = find(peers[j]);
        }
    }

    std::map<PeerId, std::vector<PeerId>> components;
    for (const auto peer : peers)
        components[find(peer)].push_back(peer);

    // Keep group ids stable: a component inherits the id most of its members already had.
    std::map<PeerId, TimeGroup> next;
    std::set<uint16_t> taken;
    for (auto& [root, members] : components)
    {
        std::map<uint16_t, int> votes;
        for (const auto peer : members)
        {
            if (auto old = m_timeGroups.find(peer); old != m_timeGroups.end())
                ++votes[old->second.id];
        }
        uint16_t id = 0;
        int best = 0;
        for (const auto& [candidate, count] : votes)
        {
            if (count > best && !taken.count(candidate))
            {
                id = candidate;
                best = count;
            }
        }
        if (id == 0)
        {
            do
            {
                id = m_nextTimeGroup;
                m_nextTimeGroup = static_cast<uint16_t>(m_nextTimeGroup == 0xFFFF ? 1 : m_nextTimeGroup + 1);
            } while (taken.count(id));
        }
        taken.insert(id);
        for (const auto peer : members)
            next[peer] = {id, members, now};
    }

    bool changed = aForceBroadcast;
    for (auto& [peer, group] : next)
    {
        auto old = m_timeGroups.find(peer);
        if (old != m_timeGroups.end() && old->second.id == group.id && old->second.members == group.members)
            group.changedAt = old->second.changedAt; // unchanged: keep the original time (no new crossfade)
        else
            changed = true;
    }
    if (next.size() != m_timeGroups.size())
        changed = true;

    m_timeGroups = std::move(next);
    if (changed)
        Broadcast(MakeMembership());
}
} // namespace coop
