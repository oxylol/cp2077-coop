#include "client/ClientSession.hpp"

#include <algorithm>
#include <cmath>

#include "core/Log.hpp"
#include "protocol/Auth.hpp"
#include "protocol/Codec.hpp"

namespace coop
{
namespace
{
constexpr TimeUs kBurstPingIntervalUs = 100 * kUsPerMs;
constexpr TimeUs kPingIntervalUs = 1 * kUsPerSecond;
constexpr int kBurstPings = 8;
constexpr TimeUs kHeartbeatIntervalUs = 2 * kUsPerSecond;
} // namespace

const char* ToString(ClientState aState)
{
    switch (aState)
    {
    case ClientState::Idle: return "idle";
    case ClientState::Connecting: return "connecting";
    case ClientState::Handshaking: return "handshaking";
    case ClientState::Authenticating: return "authenticating";
    case ClientState::Joined: return "joined";
    case ClientState::Closed: return "closed";
    }
    return "?";
}

ClientSession::ClientSession(ITransport& aTransport, IGameAdapter& aAdapter, const IClock& aClock, ClientConfig aConfig)
    : m_transport(aTransport)
    , m_adapter(aAdapter)
    , m_clock(aClock)
    , m_config(std::move(aConfig))
{
}

ClientSession::~ClientSession()
{
    if (m_conn != kInvalidConn)
        m_transport.Close(m_conn, 0, "client destroyed");
}

bool ClientSession::Connect(const std::string& aAddress, std::string& aError)
{
    if (m_state != ClientState::Idle && m_state != ClientState::Closed)
    {
        aError = "already connected or connecting";
        return false;
    }

    m_conn = m_transport.Connect(aAddress, aError);
    if (m_conn == kInvalidConn)
        return false;

    m_state = ClientState::Connecting;
    m_connectStarted = m_clock.NowUs();
    m_lastError.clear();
    m_adapter.OnStatus("connecting to " + aAddress);
    return true;
}

void ClientSession::UseConnection(ConnId aConn)
{
    m_conn = aConn;
    m_state = ClientState::Connecting;
    m_connectStarted = m_clock.NowUs();
    m_lastError.clear();
}

void ClientSession::Disconnect(const std::string& aReason)
{
    if (m_conn != kInvalidConn)
    {
        m_transport.Close(m_conn, 0, aReason.c_str());
        m_conn = kInvalidConn;
    }

    for (const auto& [peer, remote] : m_remotes)
        m_adapter.OnRemotePlayerLeft(peer);
    m_remotes.clear();
    ClearVehicles();

    if (m_state != ClientState::Closed)
    {
        m_state = ClientState::Closed;
        m_adapter.OnStatus("left the session (" + aReason + ")");
    }
}

void ClientSession::Fail(const std::string& aReason)
{
    m_lastError = aReason;
    COOP_LOG_WARN("client: %s", aReason.c_str());
    Disconnect(aReason);
    m_adapter.OnStatus(aReason);
}

template<typename M>
bool ClientSession::SendMessage(const M& aMessage)
{
    if (m_conn == kInvalidConn)
        return false;
    const auto bytes = Encode(aMessage);
    if (bytes.empty())
    {
        COOP_LOG_ERROR("client: failed to encode %s", ToString(M::kId));
        return false;
    }
    return m_transport.Send(m_conn, M::kLane, bytes, M::kReliable);
}

ConnStats ClientSession::Stats() const
{
    return m_conn != kInvalidConn ? m_transport.Stats(m_conn) : ConnStats{};
}

// ---------------------------------------------------------------------------------------------------------------------
// Tick

void ClientSession::Tick()
{
    std::vector<ConnEvent> events;
    std::vector<Packet> packets;
    m_transport.Poll(events, packets);

    for (const auto& event : events)
        HandleEvent(event);
    for (const auto& packet : packets)
    {
        if (packet.conn == m_conn)
            HandlePacket(packet);
    }

    const TimeUs now = m_clock.NowUs();

    if ((m_state == ClientState::Connecting || m_state == ClientState::Handshaking
         || m_state == ClientState::Authenticating)
        && now - m_connectStarted > m_config.connectTimeoutUs)
    {
        Fail("timed out while joining");
        return;
    }

    if (m_state != ClientState::Joined)
        return;

    SendTimeSync(now);

    if (now >= m_nextHeartbeat)
    {
        m_nextHeartbeat = now + kHeartbeatIntervalUs;
        SendMessage(msg::Heartbeat{m_heartbeatSeq++});
    }

    if (m_sync.HasEstimate())
    {
        UpdateTimeRates(SessionNow());
        SendLocalState(now);
        SendVehicleStates(now);
        DriveRemotes();
        DriveVehicles(SessionNow());
    }
}

void ClientSession::HandleEvent(const ConnEvent& aEvent)
{
    if (aEvent.conn != m_conn)
        return;

    switch (aEvent.type)
    {
    case ConnEventType::Connected:
    case ConnEventType::Incoming:
    {
        if (m_state != ClientState::Connecting)
            break;
        msg::Hello hello;
        hello.modVersion = m_config.modVersion;
        hello.gameBuild = m_config.gameBuild;
        hello.exeSize = m_config.exeSize;
        hello.manifestHash = m_config.manifestHash;
        hello.clientId = m_config.clientId;
        hello.displayName = m_config.displayName.substr(0, msg::kMaxNameLength);
        SendMessage(hello);
        m_state = ClientState::Handshaking;
        break;
    }
    case ConnEventType::Disconnected:
        m_conn = kInvalidConn;
        if (m_lastError.empty())
            Fail("connection lost: " + aEvent.reason);
        else
            Disconnect(aEvent.reason);
        break;
    }
}

void ClientSession::HandlePacket(const Packet& aPacket)
{
    EnvelopeHeader header;
    if (!PeekHeader(aPacket.data.data(), aPacket.data.size(), header))
        return;

    const uint8_t* data = aPacket.data.data();
    const size_t size = aPacket.data.size();

    switch (header.id)
    {
    case MsgId::Challenge:
    {
        msg::Challenge challenge;
        if (m_state != ClientState::Handshaking || !Decode(data, size, challenge))
            return;

        msg::AuthResponse response;
        if (challenge.passwordRequired)
        {
            if (m_config.password.empty())
            {
                Fail("this session needs a password");
                return;
            }
            const auto key = auth::DeriveKey(m_config.password, challenge.salt, challenge.iterations);
            response.proof = auth::Proof(key, challenge.nonce, m_config.clientId);
        }
        SendMessage(response);
        m_state = ClientState::Authenticating;
        return;
    }

    case MsgId::Reject:
    {
        msg::Reject reject;
        if (!Decode(data, size, reject))
            return;
        std::string reason = std::string("rejected: ") + ToString(reject.reason);
        if (!reject.detail.empty())
            reason += " (" + reject.detail + ")";
        Fail(reason);
        return;
    }

    case MsgId::Kick:
    {
        msg::Kick kick;
        if (!Decode(data, size, kick))
            return;
        Fail(std::string("removed from session: ") + ToString(kick.reason));
        return;
    }

    case MsgId::JoinAccept:
    {
        msg::JoinAccept accept;
        if (m_state != ClientState::Authenticating || !Decode(data, size, accept))
            return;
        OnJoinAccept(accept);
        return;
    }

    default:
        break;
    }

    if (m_state != ClientState::Joined)
        return;

    switch (header.id)
    {
    case MsgId::TimeSyncPong:
    {
        msg::TimeSyncPong pong;
        if (Decode(data, size, pong))
            m_sync.AddSample(pong.clientSendUs, pong.hostTimeUs, m_clock.NowUs());
        break;
    }

    case MsgId::PlayerJoined:
    {
        msg::PlayerJoined joined;
        if (Decode(data, size, joined) && joined.peer != m_peer)
            AddRemote(joined.peer, joined.name);
        break;
    }

    case MsgId::PlayerLeft:
    {
        msg::PlayerLeft left;
        if (Decode(data, size, left))
            RemoveRemote(left.peer);
        break;
    }

    case MsgId::RosterState:
    {
        msg::RosterState roster;
        if (!Decode(data, size, roster))
            break;
        for (const auto& entry : roster.entries)
        {
            auto it = m_remotes.find(entry.peer);
            if (it != m_remotes.end())
            {
                it->second.name = entry.name;
                it->second.rttMs = entry.rttMs;
            }
        }
        break;
    }

    case MsgId::PlayerState:
    {
        msg::PlayerState state;
        if (!Decode(data, size, state) || state.peer == m_peer)
            break;
        auto it = m_remotes.find(state.peer);
        if (it == m_remotes.end())
            break;

        PoseSample sample;
        sample.time = state.sessionTimeUs;
        sample.position = state.position;
        sample.velocity = state.velocity;
        sample.yaw = state.yaw;
        sample.pitch = state.pitch;
        sample.locomotion = state.locomotion;
        sample.flags = state.flags;
        sample.rate = state.rate;
        if (it->second.buffer.Push(sample))
        {
            it->second.delay.OnArrival(state.sessionTimeUs, SessionNow());
            it->second.worldOffsetUs =
                state.fieldId != 0 ? static_cast<double>(state.fieldTimeUs - state.sessionTimeUs) : 0.0;
            ++m_statesReceived;
        }
        break;
    }

    case MsgId::PlayerAppearance:
    {
        msg::PlayerAppearance appearance;
        if (!Decode(data, size, appearance) || appearance.peer == m_peer)
            break;
        LocalAppearance converted;
        converted.bodyGender = appearance.bodyGender;
        converted.customizationState = std::move(appearance.customizationState);
        converted.equipment = std::move(appearance.equipment);
        m_adapter.OnRemoteAppearance(appearance.peer, converted);
        break;
    }

    case MsgId::TimeFieldActivate:
    {
        msg::TimeFieldActivate activate;
        if (Decode(data, size, activate))
            HandleTimeFieldActivate(activate);
        break;
    }

    case MsgId::TimeFieldDeactivate:
    {
        msg::TimeFieldDeactivate deactivate;
        if (Decode(data, size, deactivate))
            m_timeClock.SetEnd(deactivate.activationId, deactivate.end);
        break;
    }

    case MsgId::TimeFieldMembership:
    {
        msg::TimeFieldMembership membership;
        if (Decode(data, size, membership))
            HandleMembership(membership);
        break;
    }

    case MsgId::VehicleSpawn:
    {
        msg::VehicleSpawn spawn;
        if (Decode(data, size, spawn))
            HandleVehicleSpawn(spawn);
        break;
    }

    case MsgId::VehicleDespawn:
    {
        msg::VehicleDespawn despawn;
        if (Decode(data, size, despawn))
            HandleVehicleDespawn(despawn);
        break;
    }

    case MsgId::VehicleSeatState:
    {
        msg::VehicleSeatState state;
        if (Decode(data, size, state))
            HandleSeatState(state);
        break;
    }

    case MsgId::VehicleState:
    {
        msg::VehicleState state;
        if (Decode(data, size, state))
            HandleVehicleState(state);
        break;
    }

    case MsgId::Chat:
    {
        msg::Chat chat;
        if (!Decode(data, size, chat))
            break;
        auto it = m_remotes.find(chat.peer);
        const std::string name = it != m_remotes.end() ? it->second.name : "player " + std::to_string(chat.peer);
        m_adapter.OnStatus(name + ": " + chat.text);
        break;
    }

    default:
        break;
    }
}

void ClientSession::OnJoinAccept(const msg::JoinAccept& aMessage)
{
    m_peer = aMessage.peer;
    m_state = ClientState::Joined;
    m_stateInterval = aMessage.stateRateHz > 0 ? kUsPerSecond / aMessage.stateRateHz : m_stateInterval;
    m_burstPingsLeft = kBurstPings;
    m_nextPing = 0;
    m_nextHeartbeat = m_clock.NowUs() + kHeartbeatIntervalUs;

    for (const auto& entry : aMessage.roster)
    {
        if (entry.peer != m_peer)
            AddRemote(entry.peer, entry.name);
    }

    SendMessage(msg::ClientReady{});

    m_appearanceSent = false;
    UpdateAppearance(m_adapter.GetLocalAppearance());

    COOP_LOG_INFO("client: joined as peer %u", static_cast<unsigned>(m_peer));
    m_adapter.OnStatus("joined the session as player " + std::to_string(m_peer));
}

void ClientSession::UpdateAppearance(const LocalAppearance& aAppearance)
{
    if (m_state != ClientState::Joined || (m_appearanceSent && aAppearance == m_sentAppearance))
        return;

    msg::PlayerAppearance message;
    message.peer = m_peer;
    message.bodyGender = aAppearance.bodyGender;
    message.customizationState = aAppearance.customizationState;
    message.equipment = aAppearance.equipment;
    if (SendMessage(message))
    {
        m_sentAppearance = aAppearance;
        m_appearanceSent = true;
    }
}

void ClientSession::AddRemote(PeerId aPeer, const std::string& aName)
{
    auto [it, inserted] = m_remotes.try_emplace(aPeer);
    it->second.name = aName;
    if (inserted)
    {
        it->second.delay = AdaptiveDelay(m_stateInterval);
        m_adapter.OnRemotePlayerJoined({aPeer, aName});
    }
}

void ClientSession::RemoveRemote(PeerId aPeer)
{
    m_timeClock.RemovePeer(aPeer);
    if (m_remotes.erase(aPeer) > 0)
        m_adapter.OnRemotePlayerLeft(aPeer);
}

// ---------------------------------------------------------------------------------------------------------------------
// Outgoing

void ClientSession::SendTimeSync(TimeUs aLocalNow)
{
    if (aLocalNow < m_nextPing)
        return;

    if (m_burstPingsLeft > 0)
    {
        --m_burstPingsLeft;
        m_nextPing = aLocalNow + kBurstPingIntervalUs;
    }
    else
    {
        m_nextPing = aLocalNow + kPingIntervalUs;
    }
    SendMessage(msg::TimeSyncPing{aLocalNow});
}

void ClientSession::SendLocalState(TimeUs aLocalNow)
{
    if (aLocalNow < m_nextStateSend)
        return;
    // Keep a steady cadence; if frames were late by more than one interval, restart from now.
    const TimeUs interval = StateSendInterval();
    m_nextStateSend += interval;
    if (m_nextStateSend <= aLocalNow)
        m_nextStateSend = aLocalNow + interval;

    LocalSample sample;
    if (!m_adapter.CaptureLocal(sample) || !sample.valid)
    {
        m_hasPrevious = false;
        return;
    }

    msg::PlayerState state;
    state.peer = m_peer;
    state.seq = m_stateSeq++;
    state.sessionTimeUs = SessionNow();
    state.position = sample.position;
    state.yaw = sample.yaw;
    state.pitch = sample.pitch;
    state.locomotion = sample.locomotion;
    state.flags = sample.flags;
    state.rate = m_timeRates.localRate;
    // Inside (or after) a time field this machine's world clock differs from session time: send both.
    const double worldTime = m_timeRates.worldTimeUs;
    if (m_timeRates.worldRate < 0.9999f || std::abs(worldTime - static_cast<double>(state.sessionTimeUs)) > 500.0)
    {
        state.fieldId = std::max<uint16_t>(1, m_timeGroup);
        state.fieldTimeUs = static_cast<TimeUs>(std::llround(worldTime));
    }

    if (m_hasPrevious && state.sessionTimeUs > m_previousTime)
    {
        const float dt = static_cast<float>(ToSeconds(state.sessionTimeUs - m_previousTime));
        state.velocity = (sample.position - m_previousPosition) * (1.0f / dt);
        // A teleport (fast travel, respawn) is not a velocity.
        if (state.velocity.Length() > quant::kVelocityMax)
            state.velocity = {};
    }
    m_hasPrevious = true;
    m_previousPosition = sample.position;
    m_previousTime = state.sessionTimeUs;

    if (SendMessage(state))
        ++m_statesSent;
}

void ClientSession::DriveRemotes()
{
    const TimeUs now = SessionNow();
    for (auto& [peer, remote] : m_remotes)
    {
        PoseSample sample;
        const auto result = remote.buffer.Sample(now - remote.delay.DelayUs(), sample);
        if (result == InterpolationBuffer::Result::Empty)
            continue;

        RemotePose pose;
        pose.position = sample.position;
        pose.velocity = sample.velocity;
        pose.yaw = sample.yaw;
        pose.pitch = sample.pitch;
        pose.speed = sample.velocity.Length2D();
        pose.locomotion = sample.locomotion;
        pose.flags = sample.flags;
        // That player's real-time rate at the moment shown: their animation plays at this speed.
        pose.rate = m_timeClock.PersonalRate(peer, now - remote.delay.DelayUs());
        pose.extrapolated = result == InterpolationBuffer::Result::Extrapolated
                         || result == InterpolationBuffer::Result::Held;

        remote.hasPose = true;
        remote.lastPose = pose;
        m_adapter.DriveRemotePlayer(peer, pose);
    }
}

std::vector<ClientSession::RemoteView> ClientSession::Remotes() const
{
    std::vector<RemoteView> views;
    for (const auto& [peer, remote] : m_remotes)
    {
        RemoteView view;
        view.peer = peer;
        view.name = remote.name;
        view.rttMs = remote.rttMs;
        view.bufferedSamples = remote.buffer.Size();
        view.delayUs = remote.delay.DelayUs();
        view.hasPose = remote.hasPose;
        view.pose = remote.lastPose;
        view.worldOffsetUs = remote.worldOffsetUs;
        views.push_back(view);
    }
    return views;
}
// ---------------------------------------------------------------------------------------------------------------------
// Vehicles

uint32_t ClientSession::RegisterLocalVehicle(uint64_t aRecord, uint64_t aAppearance, const Vec3& aPosition,
                                             const Quat& aOrientation)
{
    if (m_state != ClientState::Joined)
        return 0;

    const auto ownCount = std::count_if(m_vehicles.begin(), m_vehicles.end(), [&](const auto& aEntry)
                                        { return aEntry.second.info.spawner == m_peer; });
    if (ownCount >= static_cast<long>(msg::kMaxVehiclesPerPeer))
        return 0;

    // Ids are (peer << 24) | counter, so machines never collide; skip ids still in use after a wrap.
    uint32_t netId = 0;
    for (uint32_t attempt = 0; attempt < 0x00FFFFFFu && netId == 0; ++attempt)
    {
        const uint32_t candidate = (static_cast<uint32_t>(m_peer) << 24) | (m_nextVehicle & 0x00FFFFFFu);
        m_nextVehicle = (m_nextVehicle % 0x00FFFFFFu) + 1;
        if ((candidate & 0x00FFFFFFu) != 0 && !m_vehicles.count(candidate))
            netId = candidate;
    }
    if (netId == 0)
        return 0;

    Vehicle& vehicle = m_vehicles[netId];
    vehicle.info = {netId, m_peer, aRecord, aAppearance, aPosition, aOrientation};
    vehicle.owner = m_peer;
    vehicle.epoch = 1; // the host starts every vehicle at epoch 1 with its spawner as owner
    vehicle.local = true;
    vehicle.delay = AdaptiveDelay(m_stateInterval);

    msg::VehicleSpawn spawn;
    spawn.netId = netId;
    spawn.record = aRecord;
    spawn.appearance = aAppearance;
    spawn.position = aPosition;
    spawn.orientation = aOrientation;
    SendMessage(spawn);
    return netId;
}

void ClientSession::DespawnLocalVehicle(uint32_t aNetId)
{
    auto it = m_vehicles.find(aNetId);
    if (it == m_vehicles.end() || it->second.info.spawner != m_peer)
        return;
    // Removed when the host confirms (it refuses while someone else is inside).
    SendMessage(msg::VehicleDespawn{aNetId, msg::VehicleDespawn::Reason::Dismissed});
}

void ClientSession::RequestSeat(uint32_t aNetId, uint8_t aSeat)
{
    if (m_state == ClientState::Joined && m_vehicles.count(aNetId))
        SendMessage(msg::VehicleSeatRequest{aNetId, aSeat});
}

void ClientSession::LeaveVehicle()
{
    if (const auto seat = LocalSeat())
        SendMessage(msg::VehicleSeatRequest{seat->first, msg::kLeaveSeat});
}

std::optional<std::pair<uint32_t, uint8_t>> ClientSession::LocalSeat() const
{
    for (const auto& [netId, vehicle] : m_vehicles)
    {
        for (const auto& seat : vehicle.seats)
        {
            if (seat.peer == m_peer)
                return std::make_pair(netId, seat.seat);
        }
    }
    return std::nullopt;
}

std::vector<ClientSession::VehicleView> ClientSession::Vehicles() const
{
    std::vector<VehicleView> views;
    for (const auto& [netId, vehicle] : m_vehicles)
    {
        VehicleView view;
        view.netId = netId;
        view.record = vehicle.info.record;
        view.spawner = vehicle.info.spawner;
        view.owner = vehicle.owner;
        view.epoch = vehicle.epoch;
        view.local = vehicle.local;
        view.seats = vehicle.seats;
        view.hasPose = vehicle.hasPose;
        view.pose = vehicle.lastPose;
        views.push_back(std::move(view));
    }
    return views;
}

void ClientSession::HandleVehicleSpawn(const msg::VehicleSpawn& aSpawn)
{
    const PeerId spawner = msg::VehicleSpawner(aSpawn.netId);
    if (spawner == m_peer || m_vehicles.count(aSpawn.netId))
        return;

    Vehicle& vehicle = m_vehicles[aSpawn.netId];
    vehicle.info = {aSpawn.netId, spawner, aSpawn.record, aSpawn.appearance, aSpawn.position, aSpawn.orientation};
    vehicle.owner = spawner;
    vehicle.delay = AdaptiveDelay(m_stateInterval);
    m_adapter.OnVehicleSpawned(vehicle.info);
}

void ClientSession::HandleVehicleDespawn(const msg::VehicleDespawn& aDespawn)
{
    auto it = m_vehicles.find(aDespawn.netId);
    if (it == m_vehicles.end())
        return;
    const bool proxy = it->second.info.spawner != m_peer;
    m_vehicles.erase(it);
    // This machine's own vehicles are real game vehicles; only proxies are removed.
    if (proxy)
        m_adapter.OnVehicleDespawned(aDespawn.netId);
}

void ClientSession::HandleSeatState(const msg::VehicleSeatState& aState)
{
    auto it = m_vehicles.find(aState.netId);
    if (it == m_vehicles.end())
        return;
    Vehicle& vehicle = it->second;

    std::vector<SeatAssignment> seats;
    seats.reserve(aState.seats.size());
    for (const auto& entry : aState.seats)
        seats.push_back({entry.seat, entry.peer});

    const bool wasLocal = vehicle.local;
    vehicle.owner = aState.owner;
    if (aState.epoch != vehicle.epoch)
    {
        // A new owner's timeline: old snapshots no longer apply.
        vehicle.epoch = aState.epoch;
        vehicle.buffer.Clear();
        vehicle.hasPose = false;
    }
    vehicle.local = aState.owner == m_peer;

    if (vehicle.local != wasLocal)
        m_adapter.OnVehicleAuthority(aState.netId, vehicle.local);
    if (seats != vehicle.seats)
    {
        vehicle.seats = std::move(seats);
        m_adapter.OnVehicleSeats(aState.netId, vehicle.seats);
    }
}

void ClientSession::HandleVehicleState(const msg::VehicleState& aState)
{
    auto it = m_vehicles.find(aState.netId);
    if (it == m_vehicles.end() || it->second.local)
        return;
    Vehicle& vehicle = it->second;

    if (aState.epoch < vehicle.epoch)
        return; // the previous owner's last snapshots
    if (aState.epoch > vehicle.epoch)
    {
        // The new owner's snapshots overtook the seat update (different lanes).
        vehicle.epoch = aState.epoch;
        vehicle.buffer.Clear();
        vehicle.hasPose = false;
    }

    VehicleSample sample;
    sample.time = aState.sessionTimeUs;
    sample.position = aState.position;
    sample.orientation = aState.orientation;
    sample.velocity = aState.velocity;
    sample.steer = aState.steer;
    sample.throttle = aState.throttle;
    sample.brake = aState.brake;
    sample.flags = aState.flags;
    if (vehicle.buffer.Push(sample))
        vehicle.delay.OnArrival(aState.sessionTimeUs, SessionNow());
}

void ClientSession::SendVehicleStates(TimeUs aLocalNow)
{
    if (aLocalNow < m_nextVehicleSend)
        return;
    m_nextVehicleSend += m_stateInterval;
    if (m_nextVehicleSend <= aLocalNow)
        m_nextVehicleSend = aLocalNow + m_stateInterval;

    for (auto& [netId, vehicle] : m_vehicles)
    {
        if (!vehicle.local)
            continue;

        VehicleSample sample;
        if (!m_adapter.CaptureVehicle(netId, sample))
            continue;

        msg::VehicleState state;
        state.netId = netId;
        state.epoch = vehicle.epoch;
        state.seq = vehicle.seq++;
        state.sessionTimeUs = SessionNow();
        state.position = sample.position;
        state.orientation = sample.orientation;
        state.velocity = sample.velocity;
        if (state.velocity.Length() > quant::kVelocityMax)
            state.velocity = {};
        state.steer = Clamp(sample.steer, -1.0f, 1.0f);
        state.throttle = Clamp(sample.throttle, -1.0f, 1.0f);
        state.brake = Clamp(sample.brake, 0.0f, 1.0f);
        state.flags = sample.flags;
        SendMessage(state);
    }
}

void ClientSession::DriveVehicles(TimeUs aSessionNow)
{
    for (auto& [netId, vehicle] : m_vehicles)
    {
        if (vehicle.local)
            continue;

        VehicleSample sample;
        const auto result = vehicle.buffer.Sample(aSessionNow - vehicle.delay.DelayUs(), sample);
        if (result == VehicleBuffer::Result::Empty)
            continue;

        VehiclePose pose;
        pose.position = sample.position;
        pose.orientation = sample.orientation;
        pose.velocity = sample.velocity;
        pose.steer = sample.steer;
        pose.throttle = sample.throttle;
        pose.brake = sample.brake;
        pose.flags = sample.flags;
        pose.extrapolated = result == VehicleBuffer::Result::Extrapolated || result == VehicleBuffer::Result::Held;

        vehicle.hasPose = true;
        vehicle.lastPose = pose;
        m_adapter.DriveVehicle(netId, pose);
    }
}

void ClientSession::ClearVehicles()
{
    for (const auto& [netId, vehicle] : m_vehicles)
    {
        if (vehicle.info.spawner != m_peer)
            m_adapter.OnVehicleDespawned(netId);
        else if (!vehicle.local)
            m_adapter.OnVehicleAuthority(netId, true); // our own car, driven by someone else: it's ours again
    }
    m_vehicles.clear();
}
// ---------------------------------------------------------------------------------------------------------------------
// Time fields

uint32_t ClientSession::ActivateTimeField(msg::TimeFieldKind aKind, float aScale, TimeUs aDuration, TimeUs aEaseIn,
                                          TimeUs aEaseOut)
{
    if (m_state != ClientState::Joined || !m_sync.HasEstimate())
        return 0;

    msg::TimeFieldActivate activate;
    activate.activationId = (static_cast<uint32_t>(m_peer) << 24) | (m_nextActivation & 0x00FFFFFFu);
    m_nextActivation = (m_nextActivation % 0x00FFFFFFu) + 1;
    activate.peer = m_peer;
    activate.kind = aKind;
    activate.scaleUnits = static_cast<uint16_t>(std::clamp<long>(std::lround(aScale * msg::kTimeScaleUnits),
                                                                 msg::kMinTimeScaleUnits, msg::kTimeScaleUnits));
    activate.start = SessionNow();
    activate.end = activate.start + std::clamp<TimeUs>(aDuration, 1, msg::kMaxTimeFieldDurationUs);
    activate.easeInMs = static_cast<uint16_t>(std::clamp<TimeUs>(aEaseIn / kUsPerMs, 0, msg::kMaxTimeFieldEaseMs));
    activate.easeOutMs = static_cast<uint16_t>(std::clamp<TimeUs>(aEaseOut / kUsPerMs, 0, msg::kMaxTimeFieldEaseMs));

    // Applied from the exact values everyone else receives, so every machine computes the same curve.
    HandleTimeFieldActivate(activate);
    SendMessage(activate);
    return activate.activationId;
}

void ClientSession::CancelTimeField(uint32_t aId)
{
    auto it = m_timeClock.Activations().find(aId);
    if (it == m_timeClock.Activations().end() || it->second.peer != m_peer)
        return;
    const TimeUs now = SessionNow();
    if (now >= it->second.end)
        return;
    m_timeClock.SetEnd(aId, now);
    SendMessage(msg::TimeFieldDeactivate{aId, now});
}

void ClientSession::HandleTimeFieldActivate(const msg::TimeFieldActivate& aActivate)
{
    TimeActivation activation;
    activation.id = aActivate.activationId;
    activation.peer = aActivate.peer;
    activation.scale = static_cast<float>(aActivate.scaleUnits) / static_cast<float>(msg::kTimeScaleUnits);
    activation.start = aActivate.start;
    activation.end = aActivate.end;
    activation.easeIn = static_cast<TimeUs>(aActivate.easeInMs) * kUsPerMs;
    activation.easeOut = static_cast<TimeUs>(aActivate.easeOutMs) * kUsPerMs;
    m_timeClock.Upsert(activation);
}

void ClientSession::HandleMembership(const msg::TimeFieldMembership& aMembership)
{
    for (const auto& entry : aMembership.entries)
    {
        std::vector<PeerId> members;
        for (const auto& other : aMembership.entries)
        {
            if (other.group == entry.group)
                members.push_back(other.peer);
        }
        m_timeClock.SetGroup(entry.peer, entry.changedAt, std::move(members));
        if (entry.peer == m_peer)
            m_timeGroup = entry.group;
    }
}

double ClientSession::WorldTimeAt(TimeUs aTime) const
{
    if (!m_worldClockStarted)
        return static_cast<double>(aTime);
    if (aTime <= m_worldAnchorT)
        return m_worldAnchorW - static_cast<double>(m_worldAnchorT - aTime);
    return m_worldAnchorW + m_timeClock.IntegrateWorld(m_peer, m_worldAnchorT, aTime);
}

void ClientSession::UpdateTimeRates(TimeUs aSessionNow)
{
    if (!m_worldClockStarted)
    {
        // World time starts equal to session time.
        m_worldClockStarted = true;
        m_worldAnchorT = aSessionNow;
        m_worldAnchorW = static_cast<double>(aSessionNow);
    }

    // Keep the anchor about a second behind: late information can still correct the last second.
    constexpr TimeUs kAnchorLagUs = 1 * kUsPerSecond;
    if (aSessionNow - m_worldAnchorT > 2 * kAnchorLagUs)
    {
        const TimeUs newAnchor = aSessionNow - kAnchorLagUs;
        m_worldAnchorW += m_timeClock.IntegrateWorld(m_peer, m_worldAnchorT, newAnchor);
        m_worldAnchorT = newAnchor;
    }
    if (aSessionNow >= m_nextTimePrune)
    {
        m_nextTimePrune = aSessionNow + 5 * kUsPerSecond;
        m_timeClock.Prune(m_worldAnchorT - 5 * kUsPerSecond);
    }

    m_timeRates.sessionTimeUs = aSessionNow;
    m_timeRates.worldRate = m_timeClock.WorldRate(m_peer, aSessionNow);
    m_timeRates.localRate = m_timeClock.PersonalRate(m_peer, aSessionNow);
    m_timeRates.activating = m_timeClock.IsActivating(m_peer, aSessionNow);
    m_timeRates.group = m_timeGroup;
    m_timeRates.worldTimeUs = WorldTimeAt(aSessionNow);
    m_adapter.ApplyTimeRates(m_timeRates);
}

TimeUs ClientSession::StateSendInterval() const
{
    // Slowed players change less per real second: one sample per 33 ms of world time is enough (at most
    // 100 ms apart). An activator moves fast relative to the world and sends at 60 Hz (docs/01 §8.7).
    if (m_timeRates.activating)
        return std::min<TimeUs>(m_stateInterval, 16'667);
    if (m_timeRates.worldRate < 0.999f && m_timeRates.worldRate > 0.0f)
        return std::min<TimeUs>(static_cast<TimeUs>(static_cast<double>(m_stateInterval) / m_timeRates.worldRate),
                                100 * kUsPerMs);
    return m_stateInterval;
}
} // namespace coop
