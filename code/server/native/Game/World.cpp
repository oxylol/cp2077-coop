#include "World.h"

#include "PlayerManager.h"
#include "Level.h"

#include "Components/MovementComponent.h"
#include "Components/PlayerComponent.h"
#include "Components/AttachmentComponent.h"
#include "Components/VehicleComponent.h"

World::World()
{
    set_entity_range(1, 5'000'000);

    emplace<Level>(this);
    emplace<PlayerManager>(this);

    this->import<flecs::units>();

    component<std::string>()
        .opaque(flecs::String)
        .serialize(
            [](const flecs::serializer* s, const std::string* data)
            {
                const char* str = data->c_str();
                return s->value(flecs::String, &str); // Forward to serializer
            })
        .assign_string(
            [](std::string* data, const char* value)
            {
                *data = value; // Assign new value to std::string
            });

    MovementComponent::Register(*this);
    AttachmentComponent::Register(*this);
    PlayerComponent::Register(*this);
    VehicleComponent::Register(*this);
}

World::~World()
{
}

void World::Update(float aDelta)
{
    progress(aDelta);
}
