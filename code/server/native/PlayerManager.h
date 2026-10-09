#pragma once

struct World;
struct PlayerManager
{
    using TMap = Map<ConnectionId, flecs::entity>;

    struct Iterator
    {
        Iterator(TMap::iterator aItor)
            : m_itor(aItor)
        {
        }
        Iterator operator++()
        {
            ++m_itor;
            return *this;
        }
        bool operator!=(const Iterator& acRhs) const { return m_itor != acRhs.m_itor; }
        flecs::entity operator*() const { return m_itor->second; }
        flecs::entity operator*() { return m_itor.value(); }

    private:
        TMap::iterator m_itor;
    };


    PlayerManager(gsl::not_null<World*> apWorld);
    PlayerManager(const PlayerManager&) = delete;
    PlayerManager& operator=(const PlayerManager&) = delete;

    PlayerManager(PlayerManager&&);
    PlayerManager& operator=(PlayerManager&&);

    Iterator begin() { return Iterator(std::begin(m_players)); }
    Iterator end() { return Iterator(std::end(m_players)); }

    World* GetWorld() { return m_pWorld; }
    flecs::entity Create(ConnectionId aConnectionId, String aUsername, bool aIsHost) noexcept;
    void Remove(flecs::entity aPlayer) noexcept;
    flecs::entity GetByConnectionId(ConnectionId aConnectionId) const noexcept;
    // The player whose game hosts the session, if it has joined it.
    flecs::entity GetHost() const noexcept;

    //void SendToAll(const ServerMessage& acMessage) const noexcept;

    void SendMessageTo(flecs::entity aPlayer, String acMessage) const noexcept;
    void BroadcastMessage(String acMessage) const noexcept;

    uint32_t Count() const noexcept;

    template <class T> void ForEach(T aFunctor)
    {
        for (auto it = std::begin(m_players); it != std::end(m_players); ++it)
        {
            aFunctor(it.value());
        }
    }

private:
    TMap m_players;
    gsl::not_null<World*> m_pWorld;
};