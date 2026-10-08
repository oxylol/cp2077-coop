#include "protocol/Codec.hpp"
#include "protocol/Protocol.hpp"

namespace coop
{
bool PeekHeader(const uint8_t* aData, size_t aSize, EnvelopeHeader& aOut)
{
    if (!aData || aSize < kEnvelopeSize)
        return false;
    aOut.id = static_cast<MsgId>(static_cast<uint16_t>(aData[0] | (aData[1] << 8)));
    aOut.flags = static_cast<uint16_t>(aData[2] | (aData[3] << 8));
    return true;
}

void WriteHeader(std::vector<uint8_t>& aOut, MsgId aId, uint16_t aFlags)
{
    const auto id = static_cast<uint16_t>(aId);
    aOut.push_back(static_cast<uint8_t>(id & 0xFF));
    aOut.push_back(static_cast<uint8_t>(id >> 8));
    aOut.push_back(static_cast<uint8_t>(aFlags & 0xFF));
    aOut.push_back(static_cast<uint8_t>(aFlags >> 8));
}

const char* ToString(RejectReason aReason)
{
    switch (aReason)
    {
    case RejectReason::None: return "none";
    case RejectReason::ProtocolMismatch: return "protocol version mismatch";
    case RejectReason::ModVersionMismatch: return "mod version mismatch";
    case RejectReason::GameBuildMismatch: return "game build mismatch";
    case RejectReason::ManifestMismatch: return "mod list mismatch";
    case RejectReason::BadPassword: return "wrong password";
    case RejectReason::SessionFull: return "session is full";
    case RejectReason::Kicked: return "kicked";
    case RejectReason::HostShutdown: return "host closed the session";
    case RejectReason::Timeout: return "timed out";
    case RejectReason::Malformed: return "malformed message";
    }
    return "unknown";
}

const char* ToString(MsgId aId)
{
    switch (aId)
    {
    case MsgId::Hello: return "Hello";
    case MsgId::Challenge: return "Challenge";
    case MsgId::AuthResponse: return "AuthResponse";
    case MsgId::Reject: return "Reject";
    case MsgId::JoinAccept: return "JoinAccept";
    case MsgId::ClientReady: return "ClientReady";
    case MsgId::PlayerJoined: return "PlayerJoined";
    case MsgId::RosterState: return "RosterState";
    case MsgId::Kick: return "Kick";
    case MsgId::Heartbeat: return "Heartbeat";
    case MsgId::PlayerLeft: return "PlayerLeft";
    case MsgId::TimeSyncPing: return "TimeSyncPing";
    case MsgId::TimeSyncPong: return "TimeSyncPong";
    case MsgId::TimeFieldActivate: return "TimeFieldActivate";
    case MsgId::TimeFieldDeactivate: return "TimeFieldDeactivate";
    case MsgId::TimeFieldMembership: return "TimeFieldMembership";
    case MsgId::PlayerState: return "PlayerState";
    case MsgId::PlayerAppearance: return "PlayerAppearance";
    case MsgId::Chat: return "Chat";
    case MsgId::VehicleState: return "VehicleState";
    case MsgId::VehicleSpawn: return "VehicleSpawn";
    case MsgId::VehicleSeatRequest: return "VehicleSeatRequest";
    case MsgId::VehicleDespawn: return "VehicleDespawn";
    case MsgId::VehicleSeatState: return "VehicleSeatState";
    }
    return "Unknown";
}
} // namespace coop
