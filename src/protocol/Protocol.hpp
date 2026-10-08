#pragma once

#include <cstdint>

namespace coop
{
// Bump on any wire change; peers with different versions are rejected during the handshake.
// 2: vehicles (M0b). 3: time fields (M1). 4: animation inputs (M0b).
inline constexpr uint16_t kProtocolVersion = 4;

inline constexpr uint16_t kDefaultPort = 27077;

// GameNetworkingSockets lanes (docs/03-network-protocol.md §2).
enum class Lane : uint8_t
{
    Control = 0, // session, auth, clock, authority, facts
    Events = 1,  // combat, quest, scene, world events
    State = 2,   // unreliable snapshots
    Bulk = 3,    // baselines, save transfer
    Diag = 4,    // desync hashes, logs
    Count = 5,
};

// Message ids (docs/03-network-protocol.md). Only the M0 subset is defined so far.
enum class MsgId : uint16_t
{
    Hello = 0x0001,
    Challenge = 0x0002,
    AuthResponse = 0x0003,
    Reject = 0x0004,
    JoinAccept = 0x0006,
    ClientReady = 0x0008,
    PlayerJoined = 0x0009,
    RosterState = 0x000B,
    Kick = 0x000C,
    Heartbeat = 0x000D,
    PlayerLeft = 0x000E,

    TimeSyncPing = 0x0101,
    TimeFieldActivate = 0x0106,
    TimeFieldDeactivate = 0x0107,
    TimeFieldMembership = 0x0108,
    TimeSyncPong = 0x010C,

    PlayerState = 0x0301,
    PlayerAppearance = 0x0302,
    PlayerAnim = 0x0303,
    Chat = 0x0309,
    VehicleState = 0x0310,

    VehicleSpawn = 0x0803,
    VehicleSeatRequest = 0x0804,
    VehicleDespawn = 0x0808,
    VehicleSeatState = 0x0809,
};

enum class RejectReason : uint8_t
{
    None = 0,
    ProtocolMismatch,
    ModVersionMismatch,
    GameBuildMismatch,
    ManifestMismatch,
    BadPassword,
    SessionFull,
    Kicked,
    HostShutdown,
    Timeout,
    Malformed,
};

const char* ToString(RejectReason aReason);
const char* ToString(MsgId aId);

// Envelope: u16 message id + u16 flags (little-endian), then the bit-packed payload.
inline constexpr size_t kEnvelopeSize = 4;
inline constexpr uint16_t kFlagRelayed = 1u << 0;

// Hard upper bound on any single message; larger content goes through chunked bulk transfer (M2+).
inline constexpr size_t kMaxMessageSize = 256 * 1024;
} // namespace coop
