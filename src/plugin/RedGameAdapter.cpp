#include "plugin/RedGameAdapter.hpp"

#include <sstream>

#include "core/Log.hpp"
#include "plugin/AnimCapture.hpp"
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

uint64_t NameHash(const std::string& aName)
{
    return aName.empty() ? 0 : Red::CName(aName.c_str()).hash;
}

const char* const kMotionLabels[5] = {"speed", "direction", "vertical speed", "turn rate", "moving"};
} // namespace

float RedGameAdapter::Motion::Step()
{
    const auto now = std::chrono::steady_clock::now();
    const float dt = started ? std::chrono::duration<float>(now - last).count() : 0.0f;
    last = now;
    started = true;
    return dt;
}

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
    // The body-gender bit travels in the appearance message, not in every state update.
    m_localFemale = (sample.flags & kFemaleFlag) != 0;
    aOut.flags = static_cast<uint8_t>(sample.flags & 0x7F);
    return true;
}

LocalAppearance RedGameAdapter::GetLocalAppearance()
{
    LocalAppearance appearance;
    LocalSample sample;
    if (CaptureLocal(sample) && sample.valid)
        appearance.bodyGender = m_localFemale ? 1 : 0;
    // Customization state and equipment: spike S1b (puppets copy the local V's look for now).
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
    m_animStats.erase(aPeer);
    m_motion.erase(aPeer);
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

    // Motion inputs from the pose: the body is placed, not walked, so its graph learns how it moves from these.
    // Not for a puppet on AI walking, whose own movement already drives its graph.
    bool direct = false;
    if (m_animApply && SendsMotion() && Red::CallVirtual(bridge.instance, "IsPuppetDirect", direct, peer) && direct)
    {
        auto& motion = m_motion[aPeer];
        const float dt = motion.Step();
        const auto values = motion.tracker.Update(aPose.velocity, aPose.yaw, dt);
        if (const auto entity = PuppetEntity(aPeer))
            motion.applied += static_cast<uint64_t>(ApplyAnimInputs(entity.instance, MotionInputs(values)));
    }
}

void RedGameAdapter::ApplyTimeRates(const TimeRates& aRates)
{
    m_timeRates = aRates;
    if (const auto bridge = Bridge())
    {
        float world = aRates.worldRate;
        bool activating = aRates.activating;
        Red::CallVirtual(bridge.instance, "ApplyTimeRates", world, activating);
    }
}

void RedGameAdapter::SetAnimOptions(bool aCapture, bool aApply)
{
    m_animCapture = aCapture;
    m_animApply = aApply;
}

void RedGameAdapter::CaptureAnimInputs(std::vector<AnimInput>& aOut)
{
    if (m_animCapture)
        AnimCapture::Get().DrainForNetwork(aOut);
}

Red::Handle<Red::IScriptable> RedGameAdapter::PuppetEntity(PeerId aPeer) const
{
    Red::Handle<Red::IScriptable> entity;
    if (const auto bridge = Bridge())
    {
        auto peer = static_cast<uint32_t>(aPeer);
        Red::CallVirtual(bridge.instance, "GetPuppetEntity", entity, peer);
    }
    return entity;
}

void RedGameAdapter::ApplyRemoteAnimInputs(PeerId aPeer, const std::vector<AnimInput>& aInputs, bool /*aFull*/)
{
    auto& stats = m_animStats[aPeer];
    ++stats.messages;
    stats.received += aInputs.size();
    if (!m_animApply)
        return;
    const auto entity = PuppetEntity(aPeer);
    if (!entity)
    {
        stats.lastError = "puppet not spawned yet";
        return; // the next full set (every 2 s) catches up once it exists
    }
    std::string error;
    stats.applied += static_cast<uint64_t>(ApplyAnimInputs(entity.instance, aInputs, &error));
    if (!error.empty())
        stats.lastError = error;
}

void RedGameAdapter::SetMotionInputs(const std::array<std::string, 5>& aNames)
{
    // Per value: one or more graph input names joined with "+" (the same value goes to each, e.g.
    // "speed_horizontal+desired_speed_horizontal" for the cutscene lookalike's graph); "-name" sends it negated;
    // "" or "none" = not sent.
    auto trim = [](const std::string& aText)
    {
        const auto first = aText.find_first_not_of(" \t");
        return first == std::string::npos ? std::string() : aText.substr(first, aText.find_last_not_of(" \t") - first + 1);
    };
    uint64_t* slots[5] = {&m_motionNames.speed, &m_motionNames.direction, &m_motionNames.vertical,
                          &m_motionNames.turnRate, &m_motionNames.moving};
    m_motionExtras.clear();
    for (size_t i = 0; i < aNames.size(); ++i)
    {
        *slots[i] = 0;
        m_motionSigns[i] = 1.0f;
        m_motionNameText[i].clear();
        size_t start = 0;
        bool first = true;
        while (start <= aNames[i].size())
        {
            const auto end = aNames[i].find('+', start);
            std::string name = trim(aNames[i].substr(start, end == std::string::npos ? std::string::npos : end - start));
            start = end == std::string::npos ? aNames[i].size() + 1 : end + 1;
            if (name.empty() || name == "none" || name == "None" || name == "NONE")
                continue;
            float sign = 1.0f;
            if (name[0] == '-')
            {
                sign = -1.0f;
                name.erase(0, 1);
            }
            if (name.empty())
                continue;
            m_motionNameText[i] += (m_motionNameText[i].empty() ? "" : "+") + std::string(sign < 0.0f ? "-" : "") + name;
            if (first)
            {
                *slots[i] = NameHash(name);
                m_motionSigns[i] = sign;
                first = false;
            }
            else
            {
                m_motionExtras.push_back({i, NameHash(name), sign});
            }
        }
    }
}

std::vector<AnimInput> RedGameAdapter::MotionInputs(const MotionValues& aValues) const
{
    auto inputs = MotionToInputs(Signed(aValues), m_motionNames);
    for (const auto& extra : m_motionExtras)
    {
        AnimInput input;
        input.name = extra.name;
        if (extra.slot == 4)
        {
            input.kind = AnimInputKind::Bool;
            input.value = AnimValue::FromBool(aValues.moving);
        }
        else
        {
            const float values[4] = {aValues.speed, aValues.direction, aValues.vertical, aValues.turnRate};
            input.kind = AnimInputKind::Float;
            input.value = AnimValue::FromFloat(values[extra.slot] * extra.sign);
        }
        inputs.push_back(input);
    }
    if (m_motionFeature)
        inputs.push_back(MotionToPlayerMovement(aValues));
    if (m_tppFeature)
        inputs.push_back(TppRepresentationInput(true));
    return inputs;
}

MotionValues RedGameAdapter::Signed(MotionValues aValues) const
{
    aValues.speed *= m_motionSigns[0];
    aValues.direction *= m_motionSigns[1];
    aValues.vertical *= m_motionSigns[2];
    aValues.turnRate *= m_motionSigns[3];
    return aValues;
}

std::string RedGameAdapter::MotionStatus() const
{
    std::ostringstream out;
    out << "motion inputs:";
    if (m_motionFeature)
        out << " walking feature playerLocomotion;";
    if (m_tppFeature)
        out << " third-person feature TPPRepresentation;";
    if (!SendsMotion())
        out << " none set (coop.ini [anim] speedInput=... or the dev panel)";
    for (size_t i = 0; i < m_motionNameText.size(); ++i)
    {
        if (!m_motionNameText[i].empty())
            out << " " << kMotionLabels[i] << " -> " << m_motionNameText[i] << ";";
    }
    out << "\n";
    auto describe = [&](const char* aWho, const Motion& aMotion)
    {
        const auto& v = aMotion.tracker.Last();
        out << "  " << aWho << ": speed " << v.speed << " m/s, direction " << v.direction << " deg, vertical "
            << v.vertical << " m/s, turning " << v.turnRate << " deg/s" << (v.moving ? ", moving" : ", standing") << ", "
            << aMotion.applied << " applied\n";
    };
    if (m_mirrorMotion.started)
        describe("mirror", m_mirrorMotion);
    for (const auto& [peer, motion] : m_motion)
        describe(("player " + std::to_string(static_cast<unsigned>(peer))).c_str(), motion);
    return out.str();
}

int RedGameAdapter::DriveMirrorMotion(Red::IScriptable* aEntity)
{
    if (!aEntity || !SendsMotion())
        return 0;
    Red::Vector4 position;
    Red::Vector4 forward;
    if (!Red::CallVirtual(aEntity, "GetWorldPosition", position) || !Red::CallVirtual(aEntity, "GetWorldForward", forward))
        return 0;
    const float dt = m_mirrorMotion.Step();
    const Vec3 velocity = m_mirrorMotion.velocity.Update(FromRed(position), dt);
    const auto values = m_mirrorMotion.tracker.Update(velocity, HeadingDegrees(FromRed(forward)), dt);
    const int applied = ApplyAnimInputs(aEntity, MotionInputs(values));
    m_mirrorMotion.applied += static_cast<uint64_t>(applied);
    return applied;
}

void RedGameAdapter::ResetMirrorMotion()
{
    m_mirrorMotion = Motion{};
}

std::string RedGameAdapter::AnimApplyStatus() const
{
    std::ostringstream out;
    for (const auto& [peer, stats] : m_animStats)
    {
        out << "  from player " << static_cast<unsigned>(peer) << ": " << stats.messages << " messages, "
            << stats.received << " inputs, " << stats.applied << " applied";
        if (!stats.lastError.empty())
            out << " (last problem: " << stats.lastError << ")";
        out << "\n";
    }
    return out.str();
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
    m_animStats.clear();
    m_motion.clear();
}
} // namespace coop::plugin
