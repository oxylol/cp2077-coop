#include "SteamInterface.h"
#include "steam/steamnetworkingsockets.h"

#include <atomic>


static std::atomic<std::size_t> s_initCounter = 0;
static std::atomic<ISteamNetworkingSockets*> s_pSockets = nullptr;
static std::atomic<ISteamNetworkingUtils*> s_pUtils = nullptr;

void SteamInterface::Acquire()
{
    if (s_initCounter.fetch_add(1, std::memory_order_relaxed) == 0)
    {
        SteamDatagramErrMsg errorMessage;
        if (!GameNetworkingSockets_Init(nullptr, errorMessage))
        {
            // TODO: Error management
        }
    }
}

void SteamInterface::Release()
{
    // This seems to conflict with the game's handles so disabling it
    if (s_initCounter.fetch_sub(1, std::memory_order_relaxed) == 1)
    {
    //    GameNetworkingSockets_Kill();
    }
}


ISteamNetworkingSockets* SteamInterface::Sockets() noexcept
{
    if (auto* pSockets = s_pSockets.load())
        return pSockets;
    return SteamNetworkingSockets();
}

ISteamNetworkingUtils* SteamInterface::Utils() noexcept
{
    return UtilsFor(Sockets());
}

void SteamInterface::SetSockets(ISteamNetworkingSockets* apSockets, ISteamNetworkingUtils* apUtils) noexcept
{
    s_pUtils = apSockets ? apUtils : nullptr;
    s_pSockets = apSockets;
}

ISteamNetworkingUtils* SteamInterface::UtilsFor(const ISteamNetworkingSockets* apSockets) noexcept
{
    if (auto* pSockets = s_pSockets.load(); pSockets && pSockets == apSockets)
        return s_pUtils;
    return SteamNetworkingUtils();
}
