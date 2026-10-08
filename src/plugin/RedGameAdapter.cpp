#include "plugin/RedGameAdapter.hpp"

#include "core/Log.hpp"
#include "plugin/ScriptTypes.hpp"

namespace coop::plugin
{
namespace
{
constexpr int32_t kFemaleFlag = 0x80;

Red::Vector4 ToRed(const Vec3& aValue)
{
    Red::Vector4 result;
    result.X = aValue.x;
    result.Y = aValue.y;
    result.Z = aValue.z;
    result.W = 1.0f;
    return result;
}

Vec3 FromRed(const Red::Vector4& aValue)
{
    return {aValue.X, aValue.Y, aValue.Z};
}
} // namespace

void RedGameAdapter::SetBridge(const Red::Handle<Red::IScriptable>& aBridge)
{
    m_bridge = aBridge;
    m_warnedCapture = false;
}

bool RedGameAdapter::HasBridge() const
{
    return !m_bridge.Expired();
}

Red::Handle<Red::IScriptable> RedGameAdapter::Bridge() const
{
    return m_bridge.Lock();
}

bool RedGameAdapter::CaptureLocal(LocalSample& aOut)
{
    aOut.valid = false;
    const auto bridge = Bridge();
    if (!bridge)
        return true;

    Coop::CoopLocalSample sample;
    if (!Red::CallVirtual(bridge.instance, "CaptureLocal", sample))
    {
        if (!m_warnedCapture)
        {
            COOP_LOG_ERROR("bridge: CoopBridge.CaptureLocal() call failed (script/native signature mismatch?)");
            m_warnedCapture = true;
        }
        return true;
    }

    aOut.valid = sample.valid;
    aOut.position = FromRed(sample.position);
    aOut.yaw = sample.yaw;
    aOut.pitch = sample.pitch;
    aOut.locomotion = static_cast<uint8_t>(sample.locomotion & 0xFF);
    aOut.flags = static_cast<uint8_t>(sample.flags & 0xFF);
    return true;
}

LocalAppearance RedGameAdapter::GetLocalAppearance()
{
    LocalAppearance appearance;
    LocalSample sample;
    if (CaptureLocal(sample) && sample.valid)
        appearance.bodyGender = (sample.flags & kFemaleFlag) ? 1 : 0;
    // Customization state and equipment are filled in once spike S1 identifies the right APIs (M0b).
    return appearance;
}

void RedGameAdapter::OnRemotePlayerJoined(const RemotePlayerInfo& aInfo)
{
    m_names[aInfo.peer] = aInfo.name;
    if (const auto bridge = Bridge())
    {
        auto peer = static_cast<uint32_t>(aInfo.peer);
        Red::CString name(aInfo.name.c_str());
        Red::CallVirtual(bridge.instance, "OnPlayerJoined", peer, name);
    }
}

void RedGameAdapter::OnRemoteAppearance(PeerId aPeer, const LocalAppearance& aAppearance)
{
    m_bodyGender[aPeer] = aAppearance.bodyGender;
}

void RedGameAdapter::OnRemotePlayerLeft(PeerId aPeer)
{
    const auto name = m_names.count(aPeer) ? m_names[aPeer] : std::string("a player");
    m_names.erase(aPeer);
    m_bodyGender.erase(aPeer);
    if (const auto bridge = Bridge())
    {
        auto peer = static_cast<uint32_t>(aPeer);
        Red::CallVirtual(bridge.instance, "RemovePuppet", peer);
        Red::CString text((name + " left").c_str());
        Red::CallVirtual(bridge.instance, "ShowStatus", text);
    }
}

void RedGameAdapter::DriveRemotePlayer(PeerId aPeer, const RemotePose& aPose)
{
    const auto bridge = Bridge();
    if (!bridge)
        return;

    Coop::CoopPuppetPose pose;
    pose.position = ToRed(aPose.position);
    pose.velocity = ToRed(aPose.velocity);
    pose.velocity.W = 0.0f;
    pose.yaw = aPose.yaw;
    pose.pitch = aPose.pitch;
    pose.speed = aPose.speed;
    pose.locomotion = aPose.locomotion;
    pose.flags = aPose.flags;
    if (m_bodyGender.count(aPeer) && m_bodyGender[aPeer] == 1)
        pose.flags |= kFemaleFlag;
    pose.rate = aPose.rate;

    auto peer = static_cast<uint32_t>(aPeer);
    Red::CallVirtual(bridge.instance, "DrivePuppet", peer, pose);
}

void RedGameAdapter::OnStatus(const std::string& aText)
{
    m_lastStatus = aText;
    COOP_LOG_INFO("%s", aText.c_str());
    if (const auto bridge = Bridge())
    {
        Red::CString text(aText.c_str());
        Red::CallVirtual(bridge.instance, "ShowStatus", text);
    }
}

void RedGameAdapter::RemoveAllPuppets()
{
    if (const auto bridge = Bridge())
        Red::CallVirtual(bridge.instance, "RemoveAllPuppets");
    m_names.clear();
    m_bodyGender.clear();
}
} // namespace coop::plugin
