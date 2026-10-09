#pragma once

struct VehicleComponent
{
    uint64_t TweakDBID{0};
    // Who brought it into the session, and which vehicle it is in their game (client.EnterVehicleRequest).
    ConnectionId Creator{0};
    uint64_t GameVehicle{0};

    static void Register(flecs::world& aWorld);
};