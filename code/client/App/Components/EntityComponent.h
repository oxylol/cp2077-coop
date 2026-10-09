#pragma once
#include "Game/Animation/MultiMovementController.h"

struct EntityComponent
{
    Red::EntityID Id;
    bool IsVehicle{false};
    MultiMovementController* Controller{nullptr};
    // A vehicle of this game's own, which this player drove (VehicleSystem): not the mod's to delete.
    bool Owned{false};
};
