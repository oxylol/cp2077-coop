// Vehicle sync (docs/02-systems.md §11): spawning, snapshots, seat arbitration, ownership handoff with epochs,
// players leaving. Deterministic: in-memory transport and a manual clock, stepped at 60 frames per second.

#include <memory>
#include <random>

#include "SimWorld.hpp"
#include "Test.hpp"
#include "client/ClientSession.hpp"
#include "core/VehicleInterpolation.hpp"
#include "host/HostService.hpp"
#include "protocol/Codec.hpp"
#include "tools/sim/SimPlayer.hpp"

using namespace coop;

namespace
{
using test::Machine;
using test::World;

std::vector<SeatAssignment> HostSeats(const HostService& aHost, uint32_t aNetId)
{
    std::vector<SeatAssignment> seats;
    for (const auto& vehicle : aHost.Vehicles())
    {
        if (vehicle.netId == aNetId)
        {
            for (const auto& seat : vehicle.seats)
                seats.push_back({seat.seat, seat.peer});
        }
    }
    return seats;
}

const HostService::VehicleView* HostVehicle(const std::vector<HostService::VehicleView>& aVehicles, uint32_t aNetId)
{
    for (const auto& vehicle : aVehicles)
    {
        if (vehicle.netId == aNetId)
            return &vehicle;
    }
    return nullptr;
}

// Where aObserver's machine shows the vehicle, compared with where its simulating machine has it.
float ViewError(const Machine& aObserver, const Machine& aSimulator, uint32_t aNetId)
{
    const auto seen = aObserver.sim->VehiclePosition(aNetId);
    const auto real = aSimulator.sim->VehiclePosition(aNetId);
    if (!seen || !real)
        return 1e9f;
    return Distance(*seen, *real);
}
} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Codec and interpolation

TEST_CASE("vehicles: state message round-trips with tight precision")
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    for (int i = 0; i < 200; ++i)
    {
        msg::VehicleState in;
        in.netId = 0x02000005u;
        in.epoch = 3;
        in.seq = 77;
        in.sessionTimeUs = 123'456'789;
        in.position = {unit(rng) * 9000.0f, unit(rng) * 9000.0f, unit(rng) * 300.0f};
        in.orientation = Quat{unit(rng), unit(rng), unit(rng), unit(rng)}.Normalized();
        in.velocity = {unit(rng) * 60.0f, unit(rng) * 60.0f, unit(rng) * 10.0f};
        in.steer = unit(rng);
        in.throttle = unit(rng);
        in.brake = (unit(rng) + 1.0f) * 0.5f;
        in.flags = msg::VehicleFlags::Headlights | msg::VehicleFlags::Horn;

        const auto bytes = Encode(in);
        REQUIRE(!bytes.empty());
        msg::VehicleState out;
        REQUIRE(Decode(bytes.data(), bytes.size(), out));
        CHECK_EQ(out.netId, in.netId);
        CHECK_EQ(out.epoch, in.epoch);
        CHECK(Distance(out.position, in.position) < 0.02f);
        CHECK(AngleDegrees(out.orientation, in.orientation) < 0.05f);
        CHECK(Distance(out.velocity, in.velocity) < 0.05f);
        CHECK(std::abs(out.steer - in.steer) < 0.01f);
        CHECK(std::abs(out.brake - in.brake) < 0.02f);
        CHECK_EQ(out.flags, in.flags);
        if (i == 0)
            CHECK(bytes.size() <= 48); // 44 bytes: ~1.3 KB/s per vehicle at 30 Hz
    }
}

TEST_CASE("vehicles: seat state and spawn round-trip; TweakDBID helper")
{
    msg::VehicleSeatState in;
    in.netId = 0x01000002u;
    in.owner = 1;
    in.epoch = 9;
    in.seats = {{0, 1}, {1, 0}, {3, 2}};
    const auto bytes = Encode(in);
    msg::VehicleSeatState out;
    REQUIRE(Decode(bytes.data(), bytes.size(), out));
    CHECK(out.seats == in.seats);
    CHECK_EQ(out.epoch, 9);

    msg::VehicleSpawn spawn;
    spawn.netId = 0x01000001u;
    spawn.record = TweakDbId("Vehicle.v_standard2_archer_hella_player");
    spawn.orientation = Quat::FromYawDegrees(135.0f);
    const auto spawnBytes = Encode(spawn);
    msg::VehicleSpawn spawnOut;
    REQUIRE(Decode(spawnBytes.data(), spawnBytes.size(), spawnOut));
    CHECK_EQ(spawnOut.record, spawn.record);
    CHECK(std::abs(spawnOut.orientation.YawDegrees() - 135.0f) < 0.05f);

    CHECK_EQ(TweakDbId("Vehicle.v_standard2_archer_hella_player") >> 32, 39u); // length in the upper byte
    CHECK_EQ(msg::VehicleSpawner(0x03000010u), 3);
}

TEST_CASE("vehicles: buffer interpolates position and rotation")
{
    VehicleBuffer buffer;
    VehicleSample a;
    a.time = 0;
    a.position = {0.0f, 0.0f, 0.0f};
    a.orientation = Quat::FromYawDegrees(0.0f);
    a.velocity = {0.0f, 10.0f, 0.0f};
    VehicleSample b = a;
    b.time = 100'000;
    b.position = {0.0f, 1.0f, 0.0f};
    b.orientation = Quat::FromYawDegrees(90.0f);
    CHECK(buffer.Push(a));
    CHECK(buffer.Push(b));
    CHECK(!buffer.Push(b)); // duplicate

    VehicleSample out;
    CHECK(buffer.Sample(50'000, out) == VehicleBuffer::Result::Interpolated);
    CHECK(std::abs(out.position.y - 0.5f) < 1e-4f);
    CHECK(std::abs(out.orientation.YawDegrees() - 45.0f) < 0.5f);

    CHECK(buffer.Sample(200'000, out) == VehicleBuffer::Result::Extrapolated);
    CHECK(std::abs(out.position.y - 2.0f) < 1e-3f); // 1 m + 10 m/s * 0.1 s

    CHECK(buffer.Sample(1'000'000, out) == VehicleBuffer::Result::Held);
}

// ---------------------------------------------------------------------------------------------------------------------
// Sessions

TEST_CASE("vehicles: a summoned car appears on every machine and follows its driver")
{
    World w;
    auto& driver = w.AddClient("Jackie", sim::Script::Drive, Vec3{50.0f, 50.0f, 5.0f});
    auto& watcher = w.AddClient("Panam", sim::Script::Idle, Vec3{60.0f, 60.0f, 5.0f});
    REQUIRE(w.RunUntil([&] { return w.Ready(); }));

    REQUIRE(w.RunUntil([&] { return driver.sim->OwnVehicle() != 0; }));
    const uint32_t car = driver.sim->OwnVehicle();
    CHECK_EQ(msg::VehicleSpawner(car), driver.Peer());

    REQUIRE(w.RunUntil([&] { return watcher.sim->VehicleDrives() > 30 && w.HostPlayer().sim->VehicleDrives() > 30; }));
    CHECK_EQ(watcher.sim->KnownVehicles().at(car).record, TweakDbId("Vehicle.v_standard2_archer_hella_player"));

    w.Step(60);
    // 12 m/s, shown 70-100 ms in the past: about a metre behind.
    CHECK(ViewError(watcher, driver, car) < 2.5f);
    CHECK(ViewError(w.HostPlayer(), driver, car) < 2.5f);
    const Vec3 seen = *watcher.sim->VehiclePosition(car);
    CHECK(std::abs(Distance(Vec3{seen.x, seen.y, 5.0f}, Vec3{50.0f, 50.0f, 5.0f}) - 20.0f) < 0.5f); // on the circle

    // The host's record and every machine agree: Jackie drives, Jackie's machine simulates, epoch 1.
    const auto vehicles = w.host->Vehicles();
    REQUIRE(vehicles.size() == 1u);
    CHECK_EQ(vehicles[0].owner, driver.Peer());
    CHECK_EQ(vehicles[0].epoch, 1);
    const std::vector<SeatAssignment> expected{{0, driver.Peer()}};
    CHECK(HostSeats(*w.host, car) == expected);
    for (auto& machine : w.machines)
        CHECK(machine->sim->KnownSeats().at(car) == expected);
    CHECK(driver.sim->Simulates(car));
    CHECK(!watcher.sim->Simulates(car));

    // The driver's own player is in the car on other machines too.
    const auto& driverSeen = w.HostPlayer().sim->KnownPoses().at(driver.Peer());
    CHECK(driverSeen.flags & msg::PlayerStateFlags::InVehicle);
    CHECK(Distance(driverSeen.position, *w.HostPlayer().sim->VehiclePosition(car)) < 2.5f);
}

TEST_CASE("vehicles: the host arbitrates seats")
{
    World w;
    auto& driver = w.AddClient("Jackie", sim::Script::Drive, Vec3{50.0f, 50.0f, 5.0f});
    auto& rider = w.AddClient("Panam", sim::Script::Ride, Vec3{60.0f, 60.0f, 5.0f});
    REQUIRE(w.RunUntil([&] { return w.Ready() && driver.sim->OwnVehicle() != 0; }));
    const uint32_t car = driver.sim->OwnVehicle();

    // The rider takes the first free passenger seat.
    REQUIRE(w.RunUntil([&] { return rider.sim->Seat().has_value(); }));
    CHECK(rider.sim->Seat()->first == car);
    CHECK_EQ(rider.sim->Seat()->second, 1);

    // The host's player asks for taken seats (passenger 1, then the wheel): refused.
    auto& hostPlayer = w.HostPlayer();
    const auto deniedBefore = w.host->Stats().vehicleRequestsDenied;
    hostPlayer.session->RequestSeat(car, 1);
    hostPlayer.session->RequestSeat(car, 0);
    w.Step(10);
    CHECK_EQ(w.host->Stats().vehicleRequestsDenied, deniedBefore + 2);
    CHECK(!hostPlayer.session->LocalSeat().has_value());

    // A free seat works.
    hostPlayer.session->RequestSeat(car, 2);
    w.Step(10);
    REQUIRE(hostPlayer.session->LocalSeat().has_value());
    CHECK_EQ(hostPlayer.session->LocalSeat()->second, 2);

    // Switching seats frees the old one.
    rider.session->RequestSeat(car, 3);
    w.Step(10);
    const std::vector<SeatAssignment> expected{{0, driver.Peer()}, {2, hostPlayer.Peer()}, {3, rider.Peer()}};
    CHECK(HostSeats(*w.host, car) == expected);
    for (auto& machine : w.machines)
        CHECK(machine->sim->KnownSeats().at(car) == expected);

    // A passenger rides along: others see the rider where the car is.
    w.Step(60);
    const auto& riderSeen = driver.sim->KnownPoses().at(rider.Peer());
    CHECK(riderSeen.flags & msg::PlayerStateFlags::InVehicle);
    CHECK(Distance(riderSeen.position, *driver.sim->VehiclePosition(car)) < 3.0f);

    // The spawner can't send the car away while others are inside.
    driver.session->DespawnLocalVehicle(car);
    w.Step(10);
    CHECK_EQ(w.host->Vehicles().size(), 1u);

    // Once they're out, it can.
    rider.session->LeaveVehicle();
    hostPlayer.session->LeaveVehicle();
    w.Step(10);
    driver.session->DespawnLocalVehicle(car);
    w.Step(10);
    CHECK(w.host->Vehicles().empty());
    CHECK(rider.sim->KnownVehicles().empty());
    CHECK(hostPlayer.sim->KnownVehicles().empty());
}

TEST_CASE("vehicles: taking the wheel moves the simulation to the new driver; stale snapshots are dropped")
{
    World w;
    auto& first = w.AddClient("Jackie", sim::Script::Drive, Vec3{50.0f, 50.0f, 5.0f});
    auto& second = w.AddClient("Panam", sim::Script::Idle, Vec3{60.0f, 60.0f, 5.0f});
    REQUIRE(w.RunUntil([&] { return w.Ready() && first.sim->OwnVehicle() != 0; }));
    const uint32_t car = first.sim->OwnVehicle();
    REQUIRE(w.RunUntil([&] { return second.sim->VehicleDrives() > 10 && first.session->LocalSeat().has_value(); }));

    first.session->LeaveVehicle();
    w.Step(5);
    second.session->RequestSeat(car, 0);
    w.Step(5);

    const auto vehicles = w.host->Vehicles();
    REQUIRE(vehicles.size() == 1u);
    CHECK_EQ(vehicles[0].owner, second.Peer());
    CHECK_EQ(vehicles[0].epoch, 2);
    CHECK(second.sim->Simulates(car));
    CHECK(!first.sim->Simulates(car));

    // Everyone now follows the new driver's car.
    w.Step(90);
    CHECK(ViewError(first, second, car) < 2.5f);
    CHECK(ViewError(w.HostPlayer(), second, car) < 2.5f);

    // A snapshot from the previous owner with the old epoch (e.g. still in flight) is dropped by the host.
    msg::VehicleState stale;
    stale.netId = car;
    stale.epoch = 1;
    stale.seq = 9999;
    stale.sessionTimeUs = first.session->SessionNow();
    stale.position = {900.0f, 900.0f, 5.0f};
    const auto droppedBefore = w.host->Stats().vehicleStatesDropped;
    const auto bytes = Encode(stale);
    first.transport->Send(first.transport->FirstConnection(), Lane::State, bytes, false);
    w.Step(20);
    CHECK(w.host->Stats().vehicleStatesDropped > droppedBefore);
    CHECK(ViewError(w.HostPlayer(), second, car) < 2.5f);
}

TEST_CASE("vehicles: a leaving driver hands the car back; a leaving owner takes it along; late joiners see cars")
{
    World w;
    auto& owner = w.AddClient("Jackie", sim::Script::Drive, Vec3{50.0f, 50.0f, 5.0f});
    auto& borrower = w.AddClient("Panam", sim::Script::Idle, Vec3{60.0f, 60.0f, 5.0f});
    REQUIRE(w.RunUntil([&] { return w.Ready() && owner.sim->OwnVehicle() != 0; }));
    const uint32_t car = owner.sim->OwnVehicle();
    REQUIRE(w.RunUntil([&] { return owner.session->LocalSeat().has_value() && borrower.sim->KnownVehicles().count(car); }));

    owner.session->LeaveVehicle();
    w.Step(5);
    borrower.session->RequestSeat(car, 0);
    REQUIRE(w.RunUntil([&] { return borrower.sim->Simulates(car); }, 60));

    // A late joiner gets the car and who's in it.
    auto& late = w.AddClient("Judy", sim::Script::Idle, Vec3{70.0f, 70.0f, 5.0f});
    REQUIRE(w.RunUntil([&] { return late.session->State() == ClientState::Joined && late.sim->KnownVehicles().count(car); }));
    w.Step(30);
    const std::vector<SeatAssignment> borrowed{{0, borrower.Peer()}};
    CHECK(late.sim->KnownSeats().at(car) == borrowed);
    CHECK(late.sim->VehicleDrives() > 0);

    // The borrower disconnects mid-drive: the car goes back to its owner's machine with a new epoch.
    borrower.session->Disconnect("quit");
    w.Step(10);
    const auto vehicles = w.host->Vehicles();
    const auto* vehicle = HostVehicle(vehicles, car);
    REQUIRE(vehicle != nullptr);
    CHECK_EQ(vehicle->owner, owner.Peer());
    CHECK_EQ(vehicle->epoch, 3);
    CHECK(vehicle->seats.empty());
    CHECK(owner.sim->Simulates(car));

    // The owner leaves: their car leaves with them, on every machine.
    owner.session->Disconnect("quit");
    w.Step(10);
    CHECK(w.host->Vehicles().empty());
    CHECK(w.HostPlayer().sim->KnownVehicles().empty());
    CHECK(late.sim->KnownVehicles().empty());
}

TEST_CASE("vehicles: the host refuses forged and excess spawns")
{
    World w;
    auto& player = w.AddClient("Jackie", sim::Script::Idle, Vec3{50.0f, 50.0f, 5.0f});
    REQUIRE(w.RunUntil([&] { return w.Ready(); }));

    // Claiming another machine's id range.
    msg::VehicleSpawn forged;
    forged.netId = (static_cast<uint32_t>(w.HostPlayer().Peer()) << 24) | 5u;
    const auto bytes = Encode(forged);
    player.transport->Send(player.transport->FirstConnection(), Lane::Events, bytes, true);
    w.Step(5);
    CHECK(w.host->Vehicles().empty());

    // More than the per-player limit.
    int registered = 0;
    for (uint32_t i = 0; i < msg::kMaxVehiclesPerPeer + 3; ++i)
        registered += player.session->RegisterLocalVehicle(1, 0, {}, {}) != 0 ? 1 : 0;
    w.Step(5);
    CHECK_EQ(registered, static_cast<int>(msg::kMaxVehiclesPerPeer));
    CHECK_EQ(w.host->Vehicles().size(), msg::kMaxVehiclesPerPeer);
}
