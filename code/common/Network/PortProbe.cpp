#include "Server.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void CloseSocket(Socket aSocket)
{
    closesocket(aSocket);
}
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void CloseSocket(Socket aSocket)
{
    close(aSocket);
}
#endif

// Whether a UDP socket of this family can have the port to itself right now (IPv6 with IPv4 mapped, as the session
// listens). A family the system doesn't have can't collide.
bool CanBind(int aFamily, uint16_t aPort)
{
    const Socket sock = socket(aFamily, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSocket)
        return true;

#ifdef _WIN32
    // Without it Windows lets a socket bind a port another one already has.
    const int exclusive = 1;
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#endif

    bool bound;
    if (aFamily == AF_INET6)
    {
        const int v6only = 0;
        setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only));
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_addr = in6addr_any;
        address.sin6_port = htons(aPort);
        bound = bind(sock, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    }
    else
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(aPort);
        bound = bind(sock, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    }

    CloseSocket(sock);
    return bound;
}
} // namespace

bool Server::IsUdpPortFree(uint16_t aPort) noexcept
{
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        return true; // can't tell: let the listen socket try
#endif

    // Both families: on Windows an IPv4 socket and an IPv6 one with IPv4 mapped don't always keep each other off a
    // port, so the networking library's listen socket alone would share it.
    const bool isFree = CanBind(AF_INET, aPort) && CanBind(AF_INET6, aPort);

#ifdef _WIN32
    WSACleanup();
#endif
    return isFree;
}
