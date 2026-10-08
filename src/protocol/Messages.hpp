#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/AnimInput.hpp"
#include "core/BitStream.hpp"
#include "core/Quantize.hpp"
#include "core/Types.hpp"
#include "protocol/Protocol.hpp"

// M0 message set. Every message has:
//   kId, kLane, kReliable  -- routing metadata
//   template<S> bool Serialize(S&)  -- one function for both directions (see core/BitStream.hpp)
namespace coop::msg
{
inline constexpr size_t kMaxNameLength = 32;
inline constexpr size_t kMaxVersionLength = 32;
inline constexpr size_t kMaxBuildLength = 64;
inline constexpr size_t kMaxDetailLength = 512;
inline constexpr size_t kMaxChatLength = 256;
inline constexpr size_t kMaxAppearanceBlob = 64 * 1024;
inline constexpr int32_t kMaxEquipmentItems = 64;
inline constexpr size_t kMaxEquipmentNameLength = 128;

template<typename S, typename T, typename F>
bool SerializeVector(S& aStream, std::vector<T>& aItems, int32_t aMaxCount, F&& aItemFn)
{
    auto count = static_cast<int32_t>(aItems.size());
    if (!aStream.IntRange(count, 0, aMaxCount))
        return false;
    if constexpr (S::kIsReading)
        aItems.resize(static_cast<size_t>(count));
    for (auto& item : aItems)
    {
        if (!aItemFn(aStream, item))
            return false;
    }
    return true;
}

template<typename S>
bool PeerField(S& aStream, PeerId& aPeer)
{
    return aStream.U8(aPeer);
}

// ---------------------------------------------------------------------------------------------------------------------
// Session

struct Hello
{
    static constexpr MsgId kId = MsgId::Hello;
    static constexpr Lane kLane = Lane::Control;
    static constexpr bool kReliable = true;

    uint16_t protocolVersion = kProtocolVersion;
    std::string modVersion;
    std::string gameBuild;  // e.g. "3.0.80.51928" (exe file version) or "sim"
    uint64_t exeSize = 0;   // cheap identity check until full manifests (M5)
    Hash256 manifestHash{}; // zero until M5
    Uuid clientId{};        // persistent per installation
    std::string displayName;

    template<typename S>
    bool Serialize(S& s)
    {
        return s.U16(protocolVersion) && s.String(modVersion, kMaxVersionLength)
            && s.String(gameBuild, kMaxBuildLength) && s.U64(exeSize) && s.Array(manifestHash)
            && s.Array(clientId) && s.String(displayName, kMaxNameLength);
    }
};

struct Challenge
{
    static constexpr MsgId kId = MsgId::Challenge;
    static constexpr Lane kLane = Lane::Control;
    static constexpr bool kReliable = true;

    std::array<uint8_t, 16> salt{};
    std::array<uint8_t, 32> nonce{};
    bool passwordRequired = false;
    uint32_t iterations = 0;

    template<typename S>
    bool Serialize(S& s)
    {
        return s.Array(salt) && s.Array(nonce) && s.Bool(passwordRequired) && s.U32(iterations);
    }
};

struct AuthResponse
{
    static constexpr MsgId kId = MsgId::AuthResponse;
    static constexpr Lane kLane = Lane::Control;
    static constexpr bool kReliable = true;

    Hash256 proof{}; // HMAC-SHA256(PBKDF2(password, salt), nonce || clientId); zero without password

    template<typename S>
    bool Serialize(S& s)
    {
        return s.Array(proof);
    }
};

struct Reject
{
    static constexpr MsgId kId = MsgId::Reject;
    static constexpr Lane kLane = Lane::Control;
    static constexpr bool kReliable = true;

    RejectReason reason = RejectReason::None;
    std::string detail;

    template<typename S>
    bool Serialize(S& s)
    {
        auto value = static_cast<uint8_t>(reason);
        if (!s.U8(value))
            return false;
        reason = static_cast<RejectReason>(value);
        return s.String(detail, kMaxDetailLength);
    }
};

struct RosterEntry
{
    PeerId peer = kInvalidPeer;
    std::string name;
    uint8_t state = 0; // 0 joining, 1 playing, 2 loading (M1+)
    uint16_t rttMs = 0;

    template<typename S>
    bool Serialize(S& s)
    {
        return PeerField(s, peer) && s.String(name, kMaxNameLength) && s.U8(state) && s.U16(rttMs);
    }
};

struct JoinAccept
{
    static constexpr MsgId kId = MsgId::JoinAccept;
    static constexpr Lane kLane = Lane::Control;
    static constexpr bool kReliable = true;

    PeerId peer = kInvalidPeer;
    Uuid worldId{};
    uint16_t stateRateHz = 30;
    std::vector<RosterEntry> roster;

    template<typename S>
    bool Serialize(S& s)
    {
        return PeerField(s, peer) && s.Array(worldId) && s.U16(stateRateHz)
            && SerializeVector(s, roster, kMaxPlayers, [](S& aS, RosterEntry& aE) { return aE.Serialize(aS); });
    }
};

struct ClientReady
{
    static constexpr MsgId kId = MsgId::ClientReady;
    static constexpr Lane kLane = Lane::Control;
    static constexpr bool kReliable = true;

    template<typename S>
    bool Serialize(S&)
    {
        return true;
    }
};

struct PlayerJoined
{
    static constexpr MsgId kId = MsgId::PlayerJoined;
    static constexpr Lane kLane = Lane::Control;
    static constexpr bool kReliable = true;

    PeerId peer = kInvalidPeer;
    std::string name;

    template<typename S>
    bool Serialize(S& s)
    {
        return PeerField(s, peer) && s.String(name, kMaxNameLength);
    }
};

struct PlayerLeft
{
    static constexpr MsgId kId = MsgId::PlayerLeft;
    static constexpr Lane kLane = Lane::Control;
    static constexpr bool kReliable = true;

    PeerId peer = kInvalidPeer;
    RejectReason reason = RejectReason::None;

    template<typename S>
    bool Serialize(S& s)
    {
        auto value = static_cast<uint8_t>(reason);
        if (!PeerField(s, peer) || !s.U8(value))
            return false;
        reason = static_cast<RejectReason>(value);
        return true;
    }
};

struct RosterState
{
    static constexpr MsgId kId = MsgId::RosterState;
    static constexpr Lane kLane = Lane::Control;
    static constexpr bool kReliable = true;

    std::vector<RosterEntry> entries;

    template<typename S>
    bool Serialize(S& s)
    {
        return SerializeVector(s, entries, kMaxPlayers, [](S& aS, RosterEntry& aE) { return aE.Serialize(aS); });
    }
};

struct Kick
{
    static constexpr MsgId kId = MsgId::Kick;
    static constexpr Lane kLane = Lane::Control;
    static constexpr bool kReliable = true;

    RejectReason reason = RejectReason::Kicked;
    std::string detail;

    template<typename S>
    bool Serialize(S& s)
    {
        auto value = static_cast<uint8_t>(reason);
        if (!s.U8(value))
            return false;
        reason = static_cast<RejectReason>(value);
        return s.String(detail, kMaxDetailLength);
    }
};

struct Heartbeat
{
    static constexpr MsgId kId = MsgId::Heartbeat;
    static constexpr Lane kLane = Lane::Control;
    static constexpr bool kReliable = true;

    uint32_t seq = 0;

    template<typename S>
    bool Serialize(S& s)
    {
        return s.U32(seq);
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Clock

struct TimeSyncPing
{
    static constexpr MsgId kId = MsgId::TimeSyncPing;
    static constexpr Lane kLane = Lane::State;
    static constexpr bool kReliable = false;

    TimeUs clientSendUs = 0; // client's local clock

    template<typename S>
    bool Serialize(S& s)
    {
        return s.I64(clientSendUs);
    }
};

struct TimeSyncPong
{
    static constexpr MsgId kId = MsgId::TimeSyncPong;
    static constexpr Lane kLane = Lane::State;
    static constexpr bool kReliable = false;

    TimeUs clientSendUs = 0;
    TimeUs hostTimeUs = 0; // host session time when answering

    template<typename S>
    bool Serialize(S& s)
    {
        return s.I64(clientSendUs) && s.I64(hostTimeUs);
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Time fields: Sandevistan / Kerenzikov (docs/01-architecture.md §8)
//
// The activator applies its activation at once and announces it; the host checks it and relays it to every
// machine, so everyone knows every activation. The host groups players by proximity (TimeFieldMembership);
// each machine then computes every player's rates itself (core/TimeFieldClock.hpp). Values that enter the rate
// curves (scale, eases) are sent in exact integer units, so the activator uses exactly what others receive.
// activationId = (activator peer << 24) | counter.

inline constexpr uint16_t kTimeScaleUnits = 10'000; // scale 0.25 is sent as 2500
inline constexpr uint16_t kMinTimeScaleUnits = 500;  // 0.05
inline constexpr TimeUs kMaxTimeFieldDurationUs = 30 * kUsPerSecond;
inline constexpr uint16_t kMaxTimeFieldEaseMs = 2'000;

enum class TimeFieldKind : uint8_t
{
    Sandevistan = 0,
    Kerenzikov,
    Scripted,
};

struct TimeFieldActivate
{
    static constexpr MsgId kId = MsgId::TimeFieldActivate;
    static constexpr Lane kLane = Lane::Control;
    static constexpr bool kReliable = true;

    uint32_t activationId = 0;
    PeerId peer = kInvalidPeer; // filled in by the host
    TimeFieldKind kind = TimeFieldKind::Sandevistan;
    uint16_t scaleUnits = kTimeScaleUnits; // scale * kTimeScaleUnits
    TimeUs start = 0;                      // session time
    TimeUs end = 0;                        // session time the ease-out begins (start + duration)
    uint16_t easeInMs = 300;
    uint16_t easeOutMs = 300;

    template<typename S>
    bool Serialize(S& s)
    {
        auto kindValue = static_cast<uint8_t>(kind);
        if (!s.U32(activationId) || !PeerField(s, peer) || !s.U8(kindValue))
            return false;
        kind = static_cast<TimeFieldKind>(kindValue);
        return s.U16(scaleUnits) && s.I64(start) && s.I64(end) && s.U16(easeInMs) && s.U16(easeOutMs);
    }
};

// Ends an activation early (the activator cancels; the host on disconnect). Can only move `end` earlier.
struct TimeFieldDeactivate
{
    static constexpr MsgId kId = MsgId::TimeFieldDeactivate;
    static constexpr Lane kLane = Lane::Control;
    static constexpr bool kReliable = true;

    uint32_t activationId = 0;
    TimeUs end = 0;

    template<typename S>
    bool Serialize(S& s)
    {
        return s.U32(activationId) && s.I64(end);
    }
};

struct MembershipEntry
{
    PeerId peer = kInvalidPeer;
    uint16_t group = 0;
    TimeUs changedAt = 0; // session time this player's group last changed (starts the 300 ms crossfade)

    bool operator==(const MembershipEntry&) const = default;

    template<typename S>
    bool Serialize(S& s)
    {
        return PeerField(s, peer) && s.U16(group) && s.I64(changedAt);
    }
};

// Host to everyone: which players share a time field (are near each other). Complete list, sent on change.
struct TimeFieldMembership
{
    static constexpr MsgId kId = MsgId::TimeFieldMembership;
    static constexpr Lane kLane = Lane::Control;
    static constexpr bool kReliable = true;

    bool global = false; // host setting: everyone is always in one field
    std::vector<MembershipEntry> entries;

    template<typename S>
    bool Serialize(S& s)
    {
        return s.Bool(global)
            && SerializeVector(s, entries, kMaxPlayers, [](S& aS, MembershipEntry& aE) { return aE.Serialize(aS); });
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Players

struct PlayerState
{
    static constexpr MsgId kId = MsgId::PlayerState;
    static constexpr Lane kLane = Lane::State;
    static constexpr bool kReliable = false;

    PeerId peer = kInvalidPeer; // filled in by the host when relaying
    uint32_t seq = 0;
    TimeUs sessionTimeUs = 0;
    uint16_t fieldId = 0;     // 0 = not in a time field (M0 always 0)
    TimeUs fieldTimeUs = 0;   // only sent when fieldId != 0
    Vec3 position;
    Vec3 velocity;
    float yaw = 0.0f;
    float pitch = 0.0f;
    uint8_t locomotion = 0;   // raw player state-machine locomotion value
    uint8_t flags = 0;        // see PlayerStateFlags
    float rate = 1.0f;        // personal time rate (time fields)

    template<typename S>
    bool Serialize(S& s)
    {
        if (!PeerField(s, peer) || !s.U32(seq) || !s.I64(sessionTimeUs) || !s.U16(fieldId))
            return false;
        if (fieldId != 0 && !s.I64(fieldTimeUs))
            return false;
        return quant::Position(s, position) && quant::Velocity(s, velocity) && quant::Yaw(s, yaw)
            && quant::Pitch(s, pitch) && s.U8(locomotion) && s.U8(flags) && quant::Rate(s, rate);
    }
};

namespace PlayerStateFlags
{
inline constexpr uint8_t InVehicle = 1u << 0;
inline constexpr uint8_t Crouching = 1u << 1;
inline constexpr uint8_t Aiming = 1u << 2;
inline constexpr uint8_t InMenu = 1u << 3;
inline constexpr uint8_t Female = 1u << 7;
} // namespace PlayerStateFlags

struct PlayerAppearance
{
    static constexpr MsgId kId = MsgId::PlayerAppearance;
    static constexpr Lane kLane = Lane::Events;
    static constexpr bool kReliable = true;

    PeerId peer = kInvalidPeer;
    uint8_t bodyGender = 0; // 0 male body, 1 female body
    std::vector<uint8_t> customizationState; // opaque, format decided in S1
    std::vector<std::string> equipment;       // TweakDB item record names, visual only

    template<typename S>
    bool Serialize(S& s)
    {
        return PeerField(s, peer) && s.U8(bodyGender) && s.Blob(customizationState, kMaxAppearanceBlob)
            && SerializeVector(s, equipment, kMaxEquipmentItems,
                               [](S& aS, std::string& aItem) { return aS.String(aItem, kMaxEquipmentNameLength); });
    }
};

// Animation inputs the sender's game applied to its V (core/AnimInput.hpp). Sent at up to 15 Hz with the inputs
// that changed and every event since the last message; every 2 s with everything (`full`), so late joiners and lost
// state catch up. Reliable: events must arrive, and the volume is small.
struct PlayerAnim
{
    static constexpr MsgId kId = MsgId::PlayerAnim;
    static constexpr Lane kLane = Lane::Events;
    static constexpr bool kReliable = true;

    PeerId peer = kInvalidPeer; // filled in by the host when relaying
    TimeUs sessionTimeUs = 0;
    bool full = false;
    std::vector<AnimInput> inputs;

    template<typename S>
    bool Serialize(S& s)
    {
        return PeerField(s, peer) && s.I64(sessionTimeUs) && s.Bool(full)
            && SerializeVector(s, inputs, kMaxAnimInputsPerMessage, [](S& aS, AnimInput& aInput) { return aInput.Serialize(aS); });
    }
};

struct Chat
{
    static constexpr MsgId kId = MsgId::Chat;
    static constexpr Lane kLane = Lane::Events;
    static constexpr bool kReliable = true;

    PeerId peer = kInvalidPeer;
    std::string text;

    template<typename S>
    bool Serialize(S& s)
    {
        return PeerField(s, peer) && s.String(text, kMaxChatLength);
    }
};
// ---------------------------------------------------------------------------------------------------------------------
// Vehicles (docs/02-systems.md §11)
//
// The machine that simulates a vehicle ("owner") sends its snapshots; everyone else runs a proxy. The host
// arbitrates seats: taking the driver seat makes the driver's machine the owner and bumps the epoch, and
// snapshots with an old epoch are dropped. netId = (spawning peer << 24) | per-machine counter.

inline constexpr int32_t kMaxVehicleSeats = 8; // seat slots 0..7, 0 is the driver
inline constexpr uint8_t kDriverSeat = 0;
inline constexpr uint8_t kLeaveSeat = 0xFF;
inline constexpr uint32_t kMaxVehiclesPerPeer = 16;

inline constexpr PeerId VehicleSpawner(uint32_t aNetId)
{
    return static_cast<PeerId>(aNetId >> 24);
}

namespace VehicleFlags
{
inline constexpr uint8_t Headlights = 1u << 0;
inline constexpr uint8_t Horn = 1u << 1;
inline constexpr uint8_t Handbrake = 1u << 2;
inline constexpr uint8_t Siren = 1u << 3;
inline constexpr uint8_t Destroyed = 1u << 4;
} // namespace VehicleFlags

struct VehicleSpawn
{
    static constexpr MsgId kId = MsgId::VehicleSpawn;
    static constexpr Lane kLane = Lane::Events;
    static constexpr bool kReliable = true;

    uint32_t netId = 0;
    uint64_t record = 0;     // TweakDBID of the vehicle record
    uint64_t appearance = 0; // appearance name hash, 0 = record default
    Vec3 position;
    Quat orientation;

    template<typename S>
    bool Serialize(S& s)
    {
        return s.U32(netId) && s.U64(record) && s.U64(appearance) && quant::Position(s, position)
            && quant::Orientation(s, orientation);
    }
};

struct VehicleDespawn
{
    static constexpr MsgId kId = MsgId::VehicleDespawn;
    static constexpr Lane kLane = Lane::Events;
    static constexpr bool kReliable = true;

    enum class Reason : uint8_t
    {
        Dismissed = 0,  // the spawner sent it away
        SpawnerLeft,    // its player left the session
        Destroyed,
    };

    uint32_t netId = 0;
    Reason reason = Reason::Dismissed;

    template<typename S>
    bool Serialize(S& s)
    {
        auto value = static_cast<uint8_t>(reason);
        if (!s.U32(netId) || !s.U8(value))
            return false;
        reason = static_cast<Reason>(value);
        return true;
    }
};

// Client to host: take a seat (switching seats in the same vehicle is allowed), or kLeaveSeat to get out.
struct VehicleSeatRequest
{
    static constexpr MsgId kId = MsgId::VehicleSeatRequest;
    static constexpr Lane kLane = Lane::Events;
    static constexpr bool kReliable = true;

    uint32_t netId = 0;
    uint8_t seat = kLeaveSeat;

    template<typename S>
    bool Serialize(S& s)
    {
        return s.U32(netId) && s.U8(seat);
    }
};

struct SeatEntry
{
    uint8_t seat = 0;
    PeerId peer = kInvalidPeer;

    bool operator==(const SeatEntry&) const = default;

    template<typename S>
    bool Serialize(S& s)
    {
        return s.U8(seat) && PeerField(s, peer);
    }
};

// Host to everyone: the complete, authoritative seat and ownership state of one vehicle.
struct VehicleSeatState
{
    static constexpr MsgId kId = MsgId::VehicleSeatState;
    static constexpr Lane kLane = Lane::Events;
    static constexpr bool kReliable = true;

    uint32_t netId = 0;
    PeerId owner = kInvalidPeer; // the machine that simulates it
    uint16_t epoch = 0;          // bumped on every ownership change
    std::vector<SeatEntry> seats;

    template<typename S>
    bool Serialize(S& s)
    {
        return s.U32(netId) && PeerField(s, owner) && s.U16(epoch)
            && SerializeVector(s, seats, kMaxVehicleSeats, [](S& aS, SeatEntry& aE) { return aE.Serialize(aS); });
    }
};

struct VehicleState
{
    static constexpr MsgId kId = MsgId::VehicleState;
    static constexpr Lane kLane = Lane::State;
    static constexpr bool kReliable = false;

    uint32_t netId = 0;
    uint16_t epoch = 0;
    uint32_t seq = 0;
    TimeUs sessionTimeUs = 0;
    Vec3 position;
    Quat orientation;
    Vec3 velocity;
    float steer = 0.0f;
    float throttle = 0.0f;
    float brake = 0.0f;
    uint8_t flags = 0;

    template<typename S>
    bool Serialize(S& s)
    {
        return s.U32(netId) && s.U16(epoch) && s.U32(seq) && s.I64(sessionTimeUs) && quant::Position(s, position)
            && quant::Orientation(s, orientation) && quant::Velocity(s, velocity) && quant::SignedUnit(s, steer)
            && quant::SignedUnit(s, throttle) && quant::Unit(s, brake) && s.U8(flags);
    }
};
} // namespace coop::msg
