#include "tools/sim/SimPlayer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace coop::sim
{
namespace
{
constexpr float kPi = 3.14159265358979f;

float HeadingDegrees(const Vec3& aDirection)
{
    // Yaw convention used across the protocol: degrees, 0 = +Y (north), increasing counter-clockwise.
    // [VERIFY] against the game's convention in S1; the plugin converts at the bridge if it differs.
    return WrapDegrees(std::atan2(-aDirection.x, aDirection.y) * 180.0f / kPi);
}

Vec3 ForwardFromHeading(float aDegrees)
{
    const float radians = aDegrees * kPi / 180.0f;
    return {-std::sin(radians), std::cos(radians), 0.0f};
}

constexpr uint8_t kInVehicleFlag = 0x01; // PlayerStateFlags::InVehicle
constexpr uint8_t kHeadlights = 0x01;    // VehicleFlags::Headlights
constexpr TimeUs kSeatRetryUs = 1'000'000;
} // namespace

bool ParseScript(const std::string& aName, Script& aOut)
{
    std::string name = aName;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name == "idle") aOut = Script::Idle;
    else if (name == "circle") aOut = Script::Circle;
    else if (name == "line") aOut = Script::Line;
    else if (name == "follow") aOut = Script::Follow;
    else if (name == "drive") aOut = Script::Drive;
    else if (name == "ride") aOut = Script::Ride;
    else return false;
    return true;
}

const char* ToString(Script aScript)
{
    switch (aScript)
    {
    case Script::Idle: return "idle";
    case Script::Circle: return "circle";
    case Script::Line: return "line";
    case Script::Follow: return "follow";
    case Script::Drive: return "drive";
    case Script::Ride: return "ride";
    }
    return "?";
}

SimPlayer::SimPlayer(const IClock& aClock, SimPlayerConfig aConfig)
    : m_clock(aClock)
    , m_config(std::move(aConfig))
{
    if (m_config.center)
        m_position = *m_config.center;
}

bool SimPlayer::CaptureLocal(LocalSample& aOut)
{
    const TimeUs now = m_clock.NowUs();
    AdvanceVehicles(now);
    WantVehicles();

    // Walking happens in the bot's own time: inside a time field it really is slowed (or not, if activating).
    const float realDt = m_lastMove < 0 ? 0.0f : static_cast<float>(ToSeconds(now - m_lastMove));
    m_lastMove = now;
    const float dt = realDt * m_rates.localRate;

    if (m_config.sandevistan && m_localPeer != kInvalidPeer)
    {
        if (m_nextSandevistan < 0)
            m_nextSandevistan = now + m_config.sandevistan->firstAfter;
        if (now >= m_nextSandevistan)
        {
            m_nextSandevistan = now + m_config.sandevistan->period;
            SessionCommand command;
            command.kind = SessionCommand::Kind::TimeField;
            command.scale = m_config.sandevistan->scale;
            command.duration = m_config.sandevistan->duration;
            m_commands.push_back(command);
            if (m_config.verbose)
                std::printf("[%s] Sandevistan x%.2f for %.1f s\n", m_config.name.c_str(),
                            static_cast<double>(command.scale), ToSeconds(command.duration));
        }
    }

    // Seated: the player is wherever the vehicle is.
    if (const auto seat = Seat())
    {
        if (const auto position = VehiclePosition(seat->first))
        {
            m_position = *position;
            aOut.valid = true;
            aOut.position = m_position;
            aOut.yaw = m_yaw;
            aOut.flags = static_cast<uint8_t>(kInVehicleFlag | (m_config.female ? 0x80 : 0));
            return true;
        }
    }

    if (m_config.script == Script::Follow)
    {
        auto it = m_remotePoses.find(m_config.followPeer);
        if (it == m_remotePoses.end())
        {
            aOut.valid = false; // nobody to follow yet
            return true;
        }

        const Vec3 target = it->second.position;
        if (m_lastUpdate < 0)
        {
            // Appear a little behind the target.
            m_position = target + Vec3{m_config.followDistance, 0.0f, 0.0f};
            m_lastUpdate = now;
        }
        m_lastUpdate = now;

        const Vec3 toTarget = target - m_position;
        const float distance = toTarget.Length2D();
        if (distance > m_config.followDistance)
        {
            const float step = std::min(distance - m_config.followDistance, m_config.speed * 2.0f * dt);
            const Vec3 direction = Vec3{toTarget.x, toTarget.y, 0.0f} * (1.0f / std::max(distance, 0.001f));
            m_position += direction * step;
            m_position.z = target.z;
            m_yaw = HeadingDegrees(direction);
        }
        if (distance > 50.0f)
            m_position = target + Vec3{m_config.followDistance, 0.0f, 0.0f}; // target fast-travelled

        aOut.valid = true;
        aOut.position = m_position;
        aOut.yaw = m_yaw;
        aOut.flags = m_config.female ? 0x80 : 0;
        return true;
    }

    if (!m_config.center)
    {
        // Wait for the first remote player and walk around them.
        if (m_remotePoses.empty())
        {
            aOut.valid = false;
            return true;
        }
        m_config.center = m_remotePoses.begin()->second.position;
        if (m_config.verbose)
            std::printf("[%s] centering on (%.1f, %.1f, %.1f)\n", m_config.name.c_str(), m_config.center->x,
                        m_config.center->y, m_config.center->z);
    }

    m_botTime += dt;
    const float t = m_botTime;
    const Vec3 center = *m_config.center;

    switch (m_config.script)
    {
    case Script::Idle:
    case Script::Drive: // on foot until the car arrives
    case Script::Ride:
        m_position = center;
        break;

    case Script::Circle:
    {
        const float omega = m_config.speed / std::max(m_config.radius, 0.1f);
        const float angle = m_config.phase + omega * t;
        m_position = center + Vec3{std::cos(angle) * m_config.radius, std::sin(angle) * m_config.radius, 0.0f};
        m_yaw = HeadingDegrees({-std::sin(angle), std::cos(angle), 0.0f});
        break;
    }

    case Script::Line:
    {
        const float length = 2.0f * m_config.radius;
        const float travelled = std::fmod(m_config.speed * t + m_config.phase * m_config.radius, 2.0f * length);
        const bool forward = travelled < length;
        const float offset = forward ? travelled : 2.0f * length - travelled;
        m_position = center + Vec3{offset - m_config.radius, 0.0f, 0.0f};
        m_yaw = HeadingDegrees({forward ? 1.0f : -1.0f, 0.0f, 0.0f});
        break;
    }

    case Script::Follow: break;
    }

    aOut.valid = true;
    aOut.position = m_position;
    aOut.yaw = m_yaw;
    aOut.flags = m_config.female ? 0x80 : 0;
    return true;
}

LocalAppearance SimPlayer::GetLocalAppearance()
{
    LocalAppearance appearance;
    appearance.bodyGender = m_config.female ? 1 : 0;
    return appearance;
}

void SimPlayer::OnRemotePlayerJoined(const RemotePlayerInfo& aInfo)
{
    m_remoteNames[aInfo.peer] = aInfo.name;
    if (m_config.verbose)
        std::printf("[%s] %s joined (peer %u)\n", m_config.name.c_str(), aInfo.name.c_str(),
                    static_cast<unsigned>(aInfo.peer));
}

void SimPlayer::OnRemoteAppearance(PeerId aPeer, const LocalAppearance& aAppearance)
{
    m_appearances[aPeer] = aAppearance;
}

void SimPlayer::OnRemotePlayerLeft(PeerId aPeer)
{
    if (m_config.verbose)
        std::printf("[%s] peer %u left\n", m_config.name.c_str(), static_cast<unsigned>(aPeer));
    m_remotePoses.erase(aPeer);
    m_remoteNames.erase(aPeer);
    m_appearances.erase(aPeer);
}

void SimPlayer::DriveRemotePlayer(PeerId aPeer, const RemotePose& aPose)
{
    m_remotePoses[aPeer] = aPose;
}

void SimPlayer::OnStatus(const std::string& aText)
{
    if (m_config.verbose)
        std::printf("[%s] %s\n", m_config.name.c_str(), aText.c_str());
}
// ---------------------------------------------------------------------------------------------------------------------
// Vehicles

void SimPlayer::WantVehicles()
{
    if (m_localPeer == kInvalidPeer || !m_config.center)
        return; // not joined yet, or no idea where to be

    const TimeUs now = m_clock.NowUs();
    if (m_config.script == Script::Drive)
    {
        if (!m_spawnRequested)
        {
            m_spawnRequested = true;
            SessionCommand command;
            command.kind = SessionCommand::Kind::Spawn;
            command.record = m_config.vehicleRecord;
            command.position = *m_config.center + Vec3{m_config.radius, 0.0f, 0.0f};
            command.orientation = Quat::FromYawDegrees(0.0f); // the circle's tangent at angle 0 points along +Y
            m_commands.push_back(command);
        }
        else if (m_ownVehicle != 0 && !m_driverSeatRequested)
        {
            m_driverSeatRequested = true;
            m_commands.push_back({SessionCommand::Kind::Seat, m_ownVehicle, 0, 0, {}, {}});
        }
    }

    if (m_config.script == Script::Ride && Seat())
        m_hasRidden = true;
    if (m_config.script == Script::Ride && !m_hasRidden && !Seat() && now >= m_nextSeatRequest)
    {
        for (const auto& [netId, info] : m_vehicles)
        {
            const auto& seats = m_seats[netId];
            for (uint8_t seat = 1; seat < 4; ++seat)
            {
                const bool taken = std::any_of(seats.begin(), seats.end(),
                                               [&](const SeatAssignment& aSeat) { return aSeat.seat == seat; });
                if (!taken)
                {
                    m_commands.push_back({SessionCommand::Kind::Seat, netId, seat, 0, {}, {}});
                    m_nextSeatRequest = now + kSeatRetryUs;
                    return;
                }
            }
        }
    }
}

std::vector<SessionCommand> SimPlayer::TakeCommands()
{
    std::vector<SessionCommand> commands;
    commands.swap(m_commands);
    return commands;
}

void SimPlayer::OnVehicleRegistered(uint32_t aNetId)
{
    if (aNetId == 0)
    {
        if (m_config.verbose)
            std::printf("[%s] could not register a vehicle\n", m_config.name.c_str());
        return;
    }
    m_ownVehicle = aNetId;

    SimulatedCar car;
    car.circle = true;
    car.center = m_config.center.value_or(m_position);
    car.angle = 0.0f;
    car.position = car.center + Vec3{m_config.radius, 0.0f, 0.0f};
    car.heading = 0.0f;
    m_simulated[aNetId] = car;
    if (m_config.verbose)
        std::printf("[%s] summoned vehicle %08x\n", m_config.name.c_str(), aNetId);
}

void SimPlayer::AdvanceVehicles(TimeUs aNow)
{
    for (auto& [netId, car] : m_simulated)
    {
        if (car.last < 0)
            car.last = aNow;
        // Vehicles are world objects: they run at the world rate inside a time field.
        const float dt = static_cast<float>(ToSeconds(aNow - car.last)) * m_rates.worldRate;
        car.last = aNow;

        if (car.circle)
        {
            const float radius = std::max(m_config.radius, 1.0f);
            car.angle += m_config.vehicleSpeed / radius * dt;
            car.position = car.center + Vec3{std::cos(car.angle) * radius, std::sin(car.angle) * radius, 0.0f};
            const Vec3 tangent{-std::sin(car.angle), std::cos(car.angle), 0.0f};
            car.heading = HeadingDegrees(tangent);
            car.velocity = tangent * m_config.vehicleSpeed;
        }
        else
        {
            car.position += car.velocity * dt;
        }
        if (Seat() && Seat()->first == netId)
            m_yaw = car.heading;
    }
}

bool SimPlayer::CaptureVehicle(uint32_t aNetId, VehicleSample& aOut)
{
    AdvanceVehicles(m_clock.NowUs());
    auto it = m_simulated.find(aNetId);
    if (it == m_simulated.end())
        return false;

    const auto& car = it->second;
    aOut.position = car.position;
    aOut.orientation = Quat::FromYawDegrees(car.heading);
    aOut.velocity = car.velocity;
    aOut.throttle = car.velocity.Length() > 0.1f ? 0.6f : 0.0f;
    aOut.steer = car.circle ? -0.3f : 0.0f;
    aOut.flags = kHeadlights;
    return true;
}

void SimPlayer::OnVehicleSpawned(const VehicleInfo& aInfo)
{
    m_vehicles[aInfo.netId] = aInfo;
    if (m_config.verbose)
        std::printf("[%s] vehicle %08x appeared (player %u's)\n", m_config.name.c_str(), aInfo.netId,
                    static_cast<unsigned>(aInfo.spawner));
}

void SimPlayer::OnVehicleDespawned(uint32_t aNetId)
{
    m_vehicles.erase(aNetId);
    m_vehiclePoses.erase(aNetId);
    m_seats.erase(aNetId);
    m_simulated.erase(aNetId);
    if (m_ownVehicle == aNetId)
        m_ownVehicle = 0;
    if (m_config.verbose)
        std::printf("[%s] vehicle %08x is gone\n", m_config.name.c_str(), aNetId);
}

void SimPlayer::OnVehicleAuthority(uint32_t aNetId, bool aLocal)
{
    if (!aLocal)
    {
        m_simulated.erase(aNetId);
        return;
    }
    if (m_simulated.count(aNetId))
        return;

    // Took over a vehicle (e.g. got behind the wheel): carry on from where it is, straight ahead.
    SimulatedCar car;
    if (auto pose = m_vehiclePoses.find(aNetId); pose != m_vehiclePoses.end())
    {
        car.position = pose->second.position;
        car.heading = pose->second.orientation.YawDegrees();
    }
    else if (auto info = m_vehicles.find(aNetId); info != m_vehicles.end())
    {
        car.position = info->second.position;
        car.heading = info->second.orientation.YawDegrees();
    }
    car.velocity = ForwardFromHeading(car.heading) * (m_config.vehicleSpeed * 0.5f);
    m_simulated[aNetId] = car;
    if (m_config.verbose)
        std::printf("[%s] now driving vehicle %08x\n", m_config.name.c_str(), aNetId);
}

void SimPlayer::OnVehicleSeats(uint32_t aNetId, const std::vector<SeatAssignment>& aSeats)
{
    m_seats[aNetId] = aSeats;
}

void SimPlayer::DriveVehicle(uint32_t aNetId, const VehiclePose& aPose)
{
    m_vehiclePoses[aNetId] = aPose;
    ++m_vehicleDrives;
}

std::optional<Vec3> SimPlayer::VehiclePosition(uint32_t aNetId) const
{
    if (auto car = m_simulated.find(aNetId); car != m_simulated.end())
        return car->second.position;
    if (auto pose = m_vehiclePoses.find(aNetId); pose != m_vehiclePoses.end())
        return pose->second.position;
    if (auto info = m_vehicles.find(aNetId); info != m_vehicles.end())
        return info->second.position;
    return std::nullopt;
}

std::optional<std::pair<uint32_t, uint8_t>> SimPlayer::Seat() const
{
    if (m_localPeer == kInvalidPeer)
        return std::nullopt;
    for (const auto& [netId, seats] : m_seats)
    {
        for (const auto& seat : seats)
        {
            if (seat.peer == m_localPeer)
                return std::make_pair(netId, seat.seat);
        }
    }
    return std::nullopt;
}
void SimPlayer::ApplyTimeRates(const TimeRates& aRates)
{
    m_rates = aRates;
}

void SimPlayer::CaptureAnimInputs(std::vector<AnimInput>& aOut)
{
    if (!m_config.animOutput)
        return;

    const float speed = m_config.speed;
    m_lastSpeed = speed;

    AnimInput locomotion;
    locomotion.kind = AnimInputKind::Feature;
    locomotion.name = kSimAnimLocomotion;
    locomotion.featureClass = kSimAnimFeatureClass;
    locomotion.props.push_back({kSimAnimPropSpeed, AnimValue::FromFloat(speed)});
    locomotion.props.push_back({kSimAnimPropMoving, AnimValue::FromBool(speed > 0.1f)});
    aOut.push_back(locomotion);

    AnimInput speedInput;
    speedInput.kind = AnimInputKind::Float;
    speedInput.name = kSimAnimSpeed;
    speedInput.value = AnimValue::FromFloat(speed);
    aOut.push_back(speedInput);

    const TimeUs now = m_clock.NowUs();
    if (m_nextAnimStep < 0)
        m_nextAnimStep = now + kUsPerSecond;
    if (now >= m_nextAnimStep)
    {
        m_nextAnimStep += kUsPerSecond;
        AnimInput step;
        step.kind = AnimInputKind::Event;
        step.name = kSimAnimStep;
        aOut.push_back(step);
        ++m_animEventsProduced;
    }
}

void SimPlayer::ApplyRemoteAnimInputs(PeerId aPeer, const std::vector<AnimInput>& aInputs, bool aFull)
{
    auto& remote = m_remoteAnim[aPeer];
    ++remote.messages;
    if (aFull)
        ++remote.fullSets;
    for (const auto& input : aInputs)
    {
        if (input.kind == AnimInputKind::Event)
            ++remote.events;
        else
            remote.inputs[input.Key()] = input;
    }
}

void SimPlayer::Teleport(const Vec3& aCenter)
{
    m_config.center = aCenter;
    m_position = aCenter;
}
} // namespace coop::sim
