#pragma once

// A remote character seated in a vehicle (VehicleSystem).
struct AttachedComponent
{
    Red::EntityID Vehicle; // the vehicle's game object here
    bool Driver{false};
};