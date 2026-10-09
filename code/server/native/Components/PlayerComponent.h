#pragma once

struct PlayerComponent
{
    ConnectionId Connection;
    flecs::entity Puppet;
    std::string Username;
    bool IsHost{false};

    const char* GetUsername() const;

    static void Register(flecs::world& aWorld);
};
