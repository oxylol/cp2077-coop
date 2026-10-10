#pragma once

// A remote character seated in a vehicle (VehicleSystem).
struct AttachedComponent
{
    Red::EntityID Vehicle; // the vehicle's game object here
    bool Driver{false};
};

// A remote character got into a passenger seat of a vehicle with nobody at its wheel here: not seated yet (VehicleSystem
// seats them once someone is), it waits where it stands. Seating anyone on the passenger side of an empty car crashed
// the game (Cyberpunk2077.exe+0x1d2980), and the game slides a player who got in that way to the wheel anyway.
struct WaitingSeatComponent
{
    Red::EntityID Vehicle;
    Red::CName Seat;
};
