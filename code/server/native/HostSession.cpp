#include "HostSession.h"

#include "GameServer.h"

namespace
{
UniquePtr<GameServer> s_pServer;

// A session that just ended: its players were told, and its port stays open a moment longer so the notices get out.
UniquePtr<GameServer> s_pClosing;
std::chrono::steady_clock::time_point s_closingSince;
constexpr auto kClosingTime = std::chrono::seconds(1);
}

bool HostSession::Start(const Settings& acSettings)
{
    Stop();
    s_pClosing.reset(); // frees its port for the new session

    Config config;
    config.Port = acSettings.Port;
    config.MaxPlayer = acSettings.MaxPlayers;
    config.Password = acSettings.Password;
    config.HostToken = acSettings.HostToken;

    s_pServer = MakeUnique<GameServer>(config);
    if (!s_pServer->IsRunning())
    {
        s_pServer.reset();
        return false;
    }

    return true;
}

void HostSession::Stop()
{
    if (s_pServer)
    {
        spdlog::info("[Session] Ending the session");
        s_pServer->CloseConnections("The host ended the session");
        s_pClosing = std::move(s_pServer);
        s_closingSince = std::chrono::steady_clock::now();
    }
}

void HostSession::Update()
{
    if (s_pServer)
        s_pServer->Update();

    if (s_pClosing)
    {
        s_pClosing->Update();
        if (std::chrono::steady_clock::now() - s_closingSince >= kClosingTime)
            s_pClosing.reset();
    }
}

bool HostSession::IsRunning()
{
    return s_pServer != nullptr;
}

uint16_t HostSession::GetPort()
{
    return s_pServer ? s_pServer->GetPort() : 0;
}
