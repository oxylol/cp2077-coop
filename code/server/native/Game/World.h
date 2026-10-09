#pragma once

struct World : flecs::world
{
    TP_NOCOPYMOVE(World);

    World();
    ~World();

    void Update(float aDelta);
};
