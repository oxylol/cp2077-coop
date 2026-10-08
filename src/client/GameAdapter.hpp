#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/AnimInput.hpp"
#include "core/Math.hpp"
#include "core/Types.hpp"
#include "core/VehicleInterpolation.hpp"

namespace coop
{
// What the session needs from the game (or from a simulated player in the sim tools).
// The game plugin implements this on top of the redscript bridge; the sim tools implement it with
// scripted movement. All calls happen on the thread that ticks the ClientSession. In the game that is the
// network thread, so SessionRunner puts a queueing adapter in between and calls the game's adapter only
// from the main thread (SessionRunner::Pump).

struct LocalSample
{
    bool valid = false; // false while there is no controllable V (menus, loading)
    Vec3 position;
    float yaw = 0.0f;   // degrees
    float pitch = 0.0f; // degrees
    uint8_t locomotion = 0;
    uint8_t flags = 0;
    float personalRate = 1.0f;
};

struct LocalAppearance
{
    uint8_t bodyGender = 0; // 0 male body, 1 female body
    std::vector<uint8_t> customizationState;
    std::vector<std::string> equipment;

    bool operator==(const LocalAppearance&) const = default;
};

struct RemotePlayerInfo
{
    PeerId peer = kInvalidPeer;
    std::string name;
};

struct RemotePose
{
    Vec3 position;
    Vec3 velocity;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float speed = 0.0f; // horizontal speed, m/s
    uint8_t locomotion = 0;
    uint8_t flags = 0;
    float rate = 1.0f;
    bool extrapolated = false;
};

// Vehicles (docs/02-systems.md §11).
struct VehicleInfo
{
    uint32_t netId = 0;
    PeerId spawner = kInvalidPeer;
    uint64_t record = 0;     // TweakDBID of the vehicle record
    uint64_t appearance = 0; // 0 = record default
    Vec3 position;
    Quat orientation;
};

struct SeatAssignment
{
    uint8_t seat = 0; // 0 = driver
    PeerId peer = kInvalidPeer;

    bool operator==(const SeatAssignment&) const = default;
};

// A remote vehicle's pose for this frame (interpolated snapshot), plus control inputs for visuals.
struct VehiclePose
{
    Vec3 position;
    Quat orientation;
    Vec3 velocity;
    float steer = 0.0f;
    float throttle = 0.0f;
    float brake = 0.0f;
    uint8_t flags = 0;
    bool extrapolated = false;
};

// Time-field state for this machine, every tick (docs/01-architecture.md §8.3).
struct TimeRates
{
    float worldRate = 1.0f;   // global time dilation to apply (NPCs, physics, other players' world)
    float localRate = 1.0f;   // the local V's rate: 1 for the strongest activator, worldRate for non-activators
    bool activating = false;  // the local V has a running activation (exempt it from global dilation)
    uint16_t group = 0;       // the host's proximity group, 0 until known
    TimeUs sessionTimeUs = 0;
    double worldTimeUs = 0.0; // world time this machine should have simulated by now (clock follower target)
};

class IGameAdapter
{
public:
    virtual ~IGameAdapter() = default;

    virtual bool CaptureLocal(LocalSample& aOut) = 0;
    virtual LocalAppearance GetLocalAppearance() = 0;

    virtual void OnRemotePlayerJoined(const RemotePlayerInfo& aInfo) = 0;
    virtual void OnRemoteAppearance(PeerId aPeer, const LocalAppearance& aAppearance) = 0;
    virtual void OnRemotePlayerLeft(PeerId aPeer) = 0;

    // Called every tick for each remote player that has pose data.
    virtual void DriveRemotePlayer(PeerId aPeer, const RemotePose& aPose) = 0;

    // Human-readable status changes ("joined as peer 1", "rejected: wrong password").
    virtual void OnStatus(const std::string& aText) = 0;

    // --- Vehicles. Defaults do nothing, so adapters without vehicle support still work. ---

    // Current state of a vehicle this machine simulates (one it spawned, or one it drives).
    virtual bool CaptureVehicle(uint32_t /*aNetId*/, VehicleSample& /*aOut*/) { return false; }
    // Another machine's vehicle: spawn a proxy for it.
    virtual void OnVehicleSpawned(const VehicleInfo& /*aInfo*/) {}
    virtual void OnVehicleDespawned(uint32_t /*aNetId*/) {}
    // This machine starts (true) or stops (false) simulating the vehicle: switch it between real physics and a
    // proxy that follows DriveVehicle.
    virtual void OnVehicleAuthority(uint32_t /*aNetId*/, bool /*aLocal*/) {}
    // Who sits where; mount or unmount puppets (and the local V, when the host granted a seat).
    virtual void OnVehicleSeats(uint32_t /*aNetId*/, const std::vector<SeatAssignment>& /*aSeats*/) {}
    // Called every tick for each proxied vehicle with data.
    virtual void DriveVehicle(uint32_t /*aNetId*/, const VehiclePose& /*aPose*/) {}

    // --- Time fields. Called every tick once the clock is synced. ---
    virtual void ApplyTimeRates(const TimeRates& /*aRates*/) {}

    // --- Animation inputs (core/AnimInput.hpp). Defaults do nothing. ---
    // The inputs the game applied to the local V since the last call: the latest value of each input, and every
    // event in order. Called once per frame.
    virtual void CaptureAnimInputs(std::vector<AnimInput>& /*aOut*/) {}
    // Another player's inputs, to apply to their puppet. `aFull` marks a complete set (sent every 2 s).
    virtual void ApplyRemoteAnimInputs(PeerId /*aPeer*/, const std::vector<AnimInput>& /*aInputs*/, bool /*aFull*/) {}
};
} // namespace coop
