#include "PlayerManager.h"
#include "Game/World.h"

#include "Components/PlayerComponent.h"

PlayerManager::PlayerManager(gsl::not_null<World*> apWorld)
    : m_pWorld(apWorld)
{
}

PlayerManager::PlayerManager(PlayerManager&& aManager)
    : m_pWorld(aManager.m_pWorld)
    , m_players(std::exchange(aManager.m_players, {}))
{
}

PlayerManager& PlayerManager::operator=(PlayerManager&& aManager)
{
    std::swap(m_pWorld, aManager.m_pWorld);
    std::swap(m_players, aManager.m_players);

    return *this;
}

flecs::entity PlayerManager::Create(ConnectionId aConnectionId, String aUsername, bool aIsHost) noexcept
{
    const auto itor = m_players.find(aConnectionId);
    if (itor == std::end(m_players))
    {
        auto entity = GetWorld()->entity(fmt::format("Player {:x}", aConnectionId).c_str())
            .child_of(GetWorld()->entity("Level"))
            .emplace<PlayerComponent>(aConnectionId, flecs::entity{}, aUsername.c_str(), aIsHost);

        const auto [insertedItor, inserted] = m_players.emplace(aConnectionId, entity);
        if (inserted)
            return insertedItor.value();

        return {};
    }

    return itor.value();
}

void PlayerManager::Remove(flecs::entity aPlayer) noexcept
{
    if (auto* pPlayerComponent = aPlayer.get<PlayerComponent>())
        m_players.erase(pPlayerComponent->Connection);
}

flecs::entity PlayerManager::GetByConnectionId(ConnectionId aConnectionId) const noexcept
{
    const auto itor = m_players.find(aConnectionId);
    if (itor != std::end(m_players))
    {
        return itor.value();
    }

    return {};
}

flecs::entity PlayerManager::GetHost() const noexcept
{
    for (const auto& [connection, player] : m_players)
    {
        if (const auto* pPlayer = player.get<PlayerComponent>(); pPlayer && pPlayer->IsHost)
            return player;
    }

    return {};
}

void PlayerManager::SendMessageTo(flecs::entity aPlayer, String aMessage) const noexcept
{
    //apPlayer->SendSystemMessage(std::move(aMessage));
}

void PlayerManager::BroadcastMessage(String aMessage) const noexcept
{
}

uint32_t PlayerManager::Count() const noexcept
{
    return static_cast<uint32_t>(m_players.size());
}
