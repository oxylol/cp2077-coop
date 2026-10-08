#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "client/GameAdapter.hpp"
#include "core/Clock.hpp"
#include "core/TweakDbId.hpp"
#include "protocol/Messages.hpp"

namespace coop::sim
{
enum class Script
{
    Idle,   // stand still at the center
    Circle, // walk a circle around the center
    Line,   // walk back and forth along the X axis through the center
    Follow, // follow another player (by default the host, peer 0) at a short distance
    Drive,  // summon a car and drive it in circles around the center
    Ride,   // get into the first other player's vehicle as a passenger
};

bool ParseScript(const std::string& aName, Script& aOut);
const char* ToString(Script aScript);

struct SimPlayerConfig
{
    std::string name = "Bot";
    Script script = Script::Circle;
    std::optional<Vec3> center; // unset: use the first remote player's position when it arrives
    float radius = 6.0f;        // circle radius / half line length, meters
    float speed = 2.5f;         // m/s (walking ≈ 1.6, running ≈ 4, sprinting ≈ 6.5)
    float phase = 0.0f;         // radians, spreads several bots around the same circle
    PeerId followPeer = kHostPeer;
    float followDistance = 3.0f;
    bool female = false;
    bool verbose = true;
    float vehicleSpeed = 12.0f; // m/s when driving (~43 km/h)
    uint64_t vehicleRecord = TweakDbId("Vehicle.v_standard2_archer_hella_player");

    // Activate a Sandevistan now and then (docs/02-systems.md §6.1).
    struct Sandevistan
    {
        float scale = 0.25f;
        TimeUs duration = 8 * kUsPerSecond;
        TimeUs period = 20 * kUsPerSecond; // from one activation's start to the next
        TimeUs firstAfter = 3 * kUsPerSecond;
    };
    std::optional<Sandevistan> sandevistan;

    // Produce animation inputs like a game would (a locomotion feature, a speed input, a step event each second
    // while moving), to exercise the animation path without the game.
    bool animOutput = false;
};

// The names the sim's animation inputs use (made-up hashes; the game uses engine name hashes).
inline constexpr uint64_t kSimAnimLocomotion = 0x5133'A11E'0000'0001ull; // Feature
inline constexpr uint64_t kSimAnimFeatureClass = 0x5133'A11E'0000'0002ull;
inline constexpr uint64_t kSimAnimPropSpeed = 0x5133'A11E'0000'0003ull;
inline constexpr uint64_t kSimAnimPropMoving = 0x5133'A11E'0000'0004ull;
inline constexpr uint64_t kSimAnimSpeed = 0x5133'A11E'0000'0005ull; // Float
inline constexpr uint64_t kSimAnimStep = 0x5133'A11E'0000'0006ull;  // Event

// Something the bot wants its session to do; the owner of the session applies these (PumpCommands).
struct SessionCommand
{
    enum class Kind
    {
        Spawn,
        Seat,
        Leave,
        Despawn,
        TimeField,
    };
    Kind kind = Kind::Spawn;
    uint32_t netId = 0;
    uint8_t seat = 0;
    uint64_t record = 0;
    Vec3 position;
    Quat orientation;
    float scale = 1.0f;
    TimeUs duration = 0;
};

// A scripted fake player. Implements IGameAdapter so it can drive a real ClientSession.
class SimPlayer final : public IGameAdapter
{
public:
    SimPlayer(const IClock& aClock, SimPlayerConfig aConfig);

    bool CaptureLocal(LocalSample& aOut) override;
    LocalAppearance GetLocalAppearance() override;
    void OnRemotePlayerJoined(const RemotePlayerInfo& aInfo) override;
    void OnRemoteAppearance(PeerId aPeer, const LocalAppearance& aAppearance) override;
    void OnRemotePlayerLeft(PeerId aPeer) override;
    void DriveRemotePlayer(PeerId aPeer, const RemotePose& aPose) override;
    void OnStatus(const std::string& aText) override;

    bool CaptureVehicle(uint32_t aNetId, VehicleSample& aOut) override;
    void OnVehicleSpawned(const VehicleInfo& aInfo) override;
    void OnVehicleDespawned(uint32_t aNetId) override;
    void OnVehicleAuthority(uint32_t aNetId, bool aLocal) override;
    void OnVehicleSeats(uint32_t aNetId, const std::vector<SeatAssignment>& aSeats) override;
    void DriveVehicle(uint32_t aNetId, const VehiclePose& aPose) override;
    void ApplyTimeRates(const TimeRates& aRates) override;
    void CaptureAnimInputs(std::vector<AnimInput>& aOut) override;
    void ApplyRemoteAnimInputs(PeerId aPeer, const std::vector<AnimInput>& aInputs, bool aFull) override;

    // Commands for the session owner, and the answers.
    std::vector<SessionCommand> TakeCommands();
    void OnVehicleRegistered(uint32_t aNetId);
    void SetLocalPeer(PeerId aPeer) { m_localPeer = aPeer; }
    void SetAnimOutput(bool aOn) { m_config.animOutput = aOn; }

    [[nodiscard]] const SimPlayerConfig& Config() const { return m_config; }
    [[nodiscard]] Vec3 Position() const { return m_position; }
    [[nodiscard]] bool HasCenter() const { return m_config.center.has_value(); }
    [[nodiscard]] const std::map<PeerId, RemotePose>& KnownPoses() const { return m_remotePoses; }
    [[nodiscard]] const std::map<PeerId, std::string>& KnownNames() const { return m_remoteNames; }
    [[nodiscard]] const std::map<PeerId, LocalAppearance>& KnownAppearances() const { return m_appearances; }

    [[nodiscard]] uint32_t OwnVehicle() const { return m_ownVehicle; }
    [[nodiscard]] bool Simulates(uint32_t aNetId) const { return m_simulated.count(aNetId) != 0; }
    // Where this bot's machine has the vehicle right now (simulated, or the latest proxy pose).
    [[nodiscard]] std::optional<Vec3> VehiclePosition(uint32_t aNetId) const;
    [[nodiscard]] const std::map<uint32_t, VehicleInfo>& KnownVehicles() const { return m_vehicles; }
    [[nodiscard]] const std::map<uint32_t, std::vector<SeatAssignment>>& KnownSeats() const { return m_seats; }
    [[nodiscard]] std::optional<std::pair<uint32_t, uint8_t>> Seat() const;
    [[nodiscard]] uint64_t VehicleDrives() const { return m_vehicleDrives; }

    [[nodiscard]] const TimeRates& Rates() const { return m_rates; }

    // Animation inputs received from each remote player: the latest value per input, and counts.
    struct RemoteAnim
    {
        std::map<uint64_t, AnimInput> inputs;
        uint32_t events = 0;
        uint32_t fullSets = 0;
        uint32_t messages = 0;
    };
    [[nodiscard]] const std::map<PeerId, RemoteAnim>& KnownAnim() const { return m_remoteAnim; }
    [[nodiscard]] uint32_t AnimEventsProduced() const { return m_animEventsProduced; }
    // Jumps somewhere else (fast travel, or a test moving a player into or out of range).
    void Teleport(const Vec3& aCenter);

private:
    struct SimulatedCar
    {
        Vec3 position;
        float heading = 0.0f; // degrees
        Vec3 velocity;
        bool circle = false;
        Vec3 center;
        float angle = 0.0f; // radians around the center
        TimeUs last = -1;
    };

    void AdvanceVehicles(TimeUs aNow);
    void WantVehicles();

    const IClock& m_clock;
    SimPlayerConfig m_config;
    PeerId m_localPeer = kInvalidPeer;

    uint32_t m_ownVehicle = 0;
    bool m_spawnRequested = false;
    bool m_driverSeatRequested = false;
    bool m_hasRidden = false; // Ride: get in once; after getting out, stay out
    TimeUs m_nextSeatRequest = 0;
    std::vector<SessionCommand> m_commands;
    TimeRates m_rates;
    float m_botTime = 0.0f;    // seconds of the bot's own (possibly slowed) time spent walking its script
    TimeUs m_lastMove = -1;
    TimeUs m_nextSandevistan = -1;
    std::map<uint32_t, SimulatedCar> m_simulated;
    std::map<uint32_t, VehicleInfo> m_vehicles;
    std::map<uint32_t, VehiclePose> m_vehiclePoses;
    std::map<uint32_t, std::vector<SeatAssignment>> m_seats;
    uint64_t m_vehicleDrives = 0;

    Vec3 m_position;
    float m_yaw = 0.0f;
    TimeUs m_lastUpdate = -1;

    std::map<PeerId, RemotePose> m_remotePoses;
    std::map<PeerId, std::string> m_remoteNames;
    std::map<PeerId, LocalAppearance> m_appearances;

    std::map<PeerId, RemoteAnim> m_remoteAnim;
    TimeUs m_nextAnimStep = -1;
    uint32_t m_animEventsProduced = 0;
    float m_lastSpeed = 0.0f;
};
// Applies a bot's commands to its session (ClientSession or SessionRunner).
template<typename Session>
void PumpCommands(SimPlayer& aBot, Session& aSession)
{
    aBot.SetLocalPeer(aSession.LocalPeer());
    for (const auto& command : aBot.TakeCommands())
    {
        switch (command.kind)
        {
        case SessionCommand::Kind::Spawn:
            aBot.OnVehicleRegistered(
                aSession.RegisterLocalVehicle(command.record, 0, command.position, command.orientation));
            break;
        case SessionCommand::Kind::Seat: aSession.RequestSeat(command.netId, command.seat); break;
        case SessionCommand::Kind::Leave: aSession.LeaveVehicle(); break;
        case SessionCommand::Kind::Despawn: aSession.DespawnLocalVehicle(command.netId); break;
        case SessionCommand::Kind::TimeField:
            aSession.ActivateTimeField(msg::TimeFieldKind::Sandevistan, command.scale, command.duration);
            break;
        }
    }
}
} // namespace coop::sim
