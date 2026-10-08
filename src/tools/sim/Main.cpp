// coop-sim: headless host and fake players for testing with one game copy (docs/05-local-testing.md §3).
//
//   coop-sim host   [--port 27077] [--password P] [--name Host] [--bot circle|line|follow|idle|drive|ride|none]
//                   [--center x,y,z] [--radius m] [--speed m/s] [--impair preset] [--duration s]
//   coop-sim client --connect 127.0.0.1:27077 [--password P] [--name Bot] [--bots N]
//                   [--script circle|line|follow|idle|drive|ride] [--center x,y,z] [--radius m] [--speed m/s]
//                   [--impair preset] [--duration s]
//
// drive: summons a car and drives it in circles around the center. ride: gets into another player's car.
// --sandevistan scale,seconds,period: the bot (the first one, with --bots) activates a Sandevistan every
// `period` seconds, e.g. --sandevistan 0.25,8,20. Players within 150 m slow down with it.
//
// Impairment presets: none, lan, good, typical, bad, awful (applied to this whole process).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "client/ClientSession.hpp"
#include "core/Crypto.hpp"
#include "core/Log.hpp"
#include "core/Version.hpp"
#include "host/HostService.hpp"
#include "net/GnsTransport.hpp"
#include "tools/sim/SimPlayer.hpp"

using namespace coop;

namespace
{
std::atomic<bool> g_stop{false};

void OnSignal(int)
{
    g_stop = true;
}

struct Options
{
    std::string mode;
    uint16_t port = kDefaultPort;
    std::string connect;
    std::string password;
    std::string name;
    std::string script = "circle";
    std::string bot = "circle";
    int bots = 1;
    std::optional<Vec3> center;
    float radius = 6.0f;
    float speed = 2.5f;
    std::optional<sim::SimPlayerConfig::Sandevistan> sandevistan;
    ImpairmentPreset impair = ImpairmentPreset::None;
    double duration = 0.0; // 0 = until Ctrl+C
    bool verbose = false;
};

void PrintUsage()
{
    std::printf(
        "coop-sim %s\n"
        "  coop-sim host   [--port 27077] [--password P] [--name Host] [--bot circle|line|follow|idle|drive|ride|none]\n"
        "                  [--center x,y,z] [--radius m] [--speed m/s] [--impair preset] [--duration s]\n"
        "  coop-sim client --connect ip:port [--password P] [--name Bot] [--bots N]\n"
        "                  [--script circle|line|follow|idle|drive|ride] [--center x,y,z] [--radius m] [--speed m/s]\n"
        "                  [--impair preset] [--duration s]\n"
        "  presets: none lan good typical bad awful\n",
        kVersionString);
}

bool ParseVec3(const std::string& aText, Vec3& aOut)
{
    return std::sscanf(aText.c_str(), "%f,%f,%f", &aOut.x, &aOut.y, &aOut.z) == 3;
}

bool ParseArgs(int aArgc, char** aArgv, Options& aOut)
{
    if (aArgc < 2)
        return false;
    aOut.mode = aArgv[1];

    for (int i = 2; i < aArgc; ++i)
    {
        const std::string arg = aArgv[i];
        auto next = [&](std::string& aValue)
        {
            if (i + 1 >= aArgc)
                return false;
            aValue = aArgv[++i];
            return true;
        };

        std::string value;
        if (arg == "--verbose")
        {
            aOut.verbose = true;
            continue;
        }
        if (!next(value))
        {
            std::fprintf(stderr, "missing value for %s\n", arg.c_str());
            return false;
        }

        if (arg == "--port") aOut.port = static_cast<uint16_t>(std::atoi(value.c_str()));
        else if (arg == "--connect") aOut.connect = value;
        else if (arg == "--password") aOut.password = value;
        else if (arg == "--name") aOut.name = value;
        else if (arg == "--script") aOut.script = value;
        else if (arg == "--bot") aOut.bot = value;
        else if (arg == "--bots") aOut.bots = std::max(1, std::min(std::atoi(value.c_str()), kMaxPlayers - 1));
        else if (arg == "--radius") aOut.radius = static_cast<float>(std::atof(value.c_str()));
        else if (arg == "--speed") aOut.speed = static_cast<float>(std::atof(value.c_str()));
        else if (arg == "--sandevistan")
        {
            float scale = 0.0f;
            float seconds = 0.0f;
            float period = 0.0f;
            if (std::sscanf(value.c_str(), "%f,%f,%f", &scale, &seconds, &period) != 3 || scale <= 0.0f
                || seconds <= 0.0f || period < seconds)
            {
                std::fprintf(stderr, "--sandevistan expects scale,seconds,period (e.g. 0.25,8,20)\n");
                return false;
            }
            sim::SimPlayerConfig::Sandevistan plan;
            plan.scale = scale;
            plan.duration = FromSeconds(seconds);
            plan.period = FromSeconds(period);
            aOut.sandevistan = plan;
        }
        else if (arg == "--duration") aOut.duration = std::atof(value.c_str());
        else if (arg == "--center")
        {
            Vec3 center;
            if (!ParseVec3(value, center))
            {
                std::fprintf(stderr, "--center expects x,y,z\n");
                return false;
            }
            aOut.center = center;
        }
        else if (arg == "--impair")
        {
            if (!ParseImpairmentPreset(value, aOut.impair))
            {
                std::fprintf(stderr, "unknown impairment preset '%s'\n", value.c_str());
                return false;
            }
        }
        else
        {
            std::fprintf(stderr, "unknown option %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

Uuid RandomUuid()
{
    Uuid id{};
    crypto::RandomBytes(id.data(), id.size());
    return id;
}

bool TimeUp(const Options& aOptions, const SteadyClock& aClock)
{
    return g_stop || (aOptions.duration > 0.0 && ToSeconds(aClock.NowUs()) >= aOptions.duration);
}

void PrintClient(const ClientSession& aSession, const sim::SimPlayer& aPlayer)
{
    const auto stats = aSession.Stats();
    std::printf("[%s] %s peer=%u ping=%dms clock=%s sent=%u recv=%u", aPlayer.Config().name.c_str(),
                ToString(aSession.State()), static_cast<unsigned>(aSession.LocalPeer()), stats.pingMs,
                aSession.HasSessionTime() ? "synced" : "syncing", aSession.StatesSent(), aSession.StatesReceived());
    for (const auto& remote : aSession.Remotes())
    {
        std::printf(" | %s#%u", remote.name.c_str(), static_cast<unsigned>(remote.peer));
        if (remote.hasPose)
            std::printf(" (%.1f, %.1f, %.1f) delay=%lldms", remote.pose.position.x, remote.pose.position.y,
                        remote.pose.position.z, static_cast<long long>(remote.delayUs / kUsPerMs));
    }
    const auto rates = aSession.CurrentTimeRates();
    if (rates.worldRate < 0.999f || rates.activating)
        std::printf(" | time: world x%.2f, me x%.2f%s", static_cast<double>(rates.worldRate),
                    static_cast<double>(rates.localRate), rates.activating ? " (Sandevistan)" : "");
    for (const auto& vehicle : aSession.Vehicles())
    {
        std::printf(" | car %08x owner#%u epoch %u%s seats:", vehicle.netId, static_cast<unsigned>(vehicle.owner),
                    static_cast<unsigned>(vehicle.epoch), vehicle.local ? " (simulated here)" : "");
        for (const auto& seat : vehicle.seats)
            std::printf(" %u=#%u", static_cast<unsigned>(seat.seat), static_cast<unsigned>(seat.peer));
        if (vehicle.hasPose)
            std::printf(" at (%.1f, %.1f)", vehicle.pose.position.x, vehicle.pose.position.y);
    }
    std::printf("\n");
}

sim::SimPlayerConfig MakePlayerConfig(const Options& aOptions, const std::string& aScript, const std::string& aName,
                                      int aIndex, int aCount)
{
    sim::SimPlayerConfig config;
    config.name = aName;
    if (!sim::ParseScript(aScript, config.script))
        config.script = sim::Script::Circle;
    config.center = aOptions.center;
    config.radius = aOptions.radius;
    config.speed = aOptions.speed;
    config.phase = aCount > 0 ? 2.0f * 3.14159265f * static_cast<float>(aIndex) / static_cast<float>(aCount) : 0.0f;
    config.followDistance = 2.5f + 1.5f * static_cast<float>(aIndex);
    config.female = aIndex % 2 == 1;
    if (aIndex == 0)
        config.sandevistan = aOptions.sandevistan;
    return config;
}

int RunHost(const Options& aOptions)
{
    SteadyClock clock;
    GnsTransport hostTransport;

    HostConfig config;
    config.port = aOptions.port;
    config.hostName = aOptions.name.empty() ? "SimHost" : aOptions.name;
    config.password = aOptions.password;
    config.gameBuild = "sim";
    config.requireSameBuild = false; // a sim host accepts real game clients of any build

    HostService host(hostTransport, clock, config);
    std::string error;
    if (!host.Start(error))
    {
        std::fprintf(stderr, "could not start host: %s\n", error.c_str());
        return 1;
    }
    std::printf("hosting on UDP port %u%s\n", static_cast<unsigned>(aOptions.port),
                aOptions.password.empty() ? "" : " (password set)");

    // Optional scripted host player, connected in-process like the game's own host player.
    std::unique_ptr<GnsTransport> botTransport;
    std::unique_ptr<sim::SimPlayer> botPlayer;
    std::unique_ptr<ClientSession> botSession;
    if (aOptions.bot != "none")
    {
        botTransport = std::make_unique<GnsTransport>();
        botPlayer = std::make_unique<sim::SimPlayer>(
            clock, MakePlayerConfig(aOptions, aOptions.bot, config.hostName, 0, 1));

        ClientConfig clientConfig;
        clientConfig.displayName = config.hostName;
        clientConfig.gameBuild = "sim";
        clientConfig.clientId = RandomUuid();
        botSession = std::make_unique<ClientSession>(*botTransport, *botPlayer, clock, clientConfig);

        ConnId hostSide = kInvalidConn;
        ConnId botSide = kInvalidConn;
        if (!GnsTransport::CreatePair(hostTransport, hostSide, *botTransport, botSide))
        {
            std::fprintf(stderr, "could not create in-process connection\n");
            return 1;
        }
        host.MarkLocal(hostSide);
        host.AdoptConnection(hostSide);
        botSession->UseConnection(botSide);
    }

    TimeUs nextPrint = 0;
    while (!TimeUp(aOptions, clock))
    {
        host.Tick();
        if (botSession)
        {
            botSession->Tick();
            sim::PumpCommands(*botPlayer, *botSession);
        }

        if (clock.NowUs() >= nextPrint)
        {
            nextPrint = clock.NowUs() + 2 * kUsPerSecond;
            std::printf("[host] players=%zu relayed=%llu rejected=%llu |", host.JoinedCount(),
                        static_cast<unsigned long long>(host.Stats().statesRelayed),
                        static_cast<unsigned long long>(host.Stats().rejected));
            for (const auto& entry : host.Roster())
                std::printf(" %s#%u %ums", entry.name.c_str(), static_cast<unsigned>(entry.peer), entry.rttMs);
            std::printf("\n");
            if (botSession && aOptions.verbose)
                PrintClient(*botSession, *botPlayer);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    host.Stop();
    for (int i = 0; i < 10; ++i)
    {
        host.Tick();
        if (botSession)
            botSession->Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return 0;
}

int RunClients(const Options& aOptions)
{
    if (aOptions.connect.empty())
    {
        std::fprintf(stderr, "client mode needs --connect ip:port\n");
        return 1;
    }

    SteadyClock clock;
    struct Bot
    {
        std::unique_ptr<GnsTransport> transport;
        std::unique_ptr<sim::SimPlayer> player;
        std::unique_ptr<ClientSession> session;
    };
    std::vector<Bot> bots;

    for (int i = 0; i < aOptions.bots; ++i)
    {
        Bot bot;
        const std::string name = (aOptions.name.empty() ? "Bot" : aOptions.name)
                               + (aOptions.bots > 1 ? std::to_string(i + 1) : "");
        bot.transport = std::make_unique<GnsTransport>();
        bot.player = std::make_unique<sim::SimPlayer>(clock, MakePlayerConfig(aOptions, aOptions.script, name, i,
                                                                               aOptions.bots));
        ClientConfig config;
        config.displayName = name;
        config.password = aOptions.password;
        config.gameBuild = "sim";
        config.clientId = RandomUuid();
        bot.session = std::make_unique<ClientSession>(*bot.transport, *bot.player, clock, config);

        std::string error;
        if (!bot.session->Connect(aOptions.connect, error))
        {
            std::fprintf(stderr, "[%s] %s\n", name.c_str(), error.c_str());
            return 1;
        }
        bots.push_back(std::move(bot));
    }

    TimeUs nextPrint = 0;
    while (!TimeUp(aOptions, clock))
    {
        bool anyOpen = false;
        for (auto& bot : bots)
        {
            bot.session->Tick();
            sim::PumpCommands(*bot.player, *bot.session);
            anyOpen |= bot.session->State() != ClientState::Closed;
        }
        if (!anyOpen)
        {
            std::printf("all players disconnected\n");
            return 2;
        }

        if (clock.NowUs() >= nextPrint)
        {
            nextPrint = clock.NowUs() + 2 * kUsPerSecond;
            for (const auto& bot : bots)
                PrintClient(*bot.session, *bot.player);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    for (auto& bot : bots)
        bot.session->Disconnect("sim stopped");
    return 0;
}
} // namespace

int main(int aArgc, char** aArgv)
{
    Options options;
    if (!ParseArgs(aArgc, aArgv, options) || (options.mode != "host" && options.mode != "client"))
    {
        PrintUsage();
        return 1;
    }

    std::signal(SIGINT, OnSignal);
    Log::SetMinLevel(options.verbose ? LogLevel::Debug : LogLevel::Info);
    Log::SetSink([](LogLevel aLevel, const std::string& aMessage)
                 { std::printf("[%s] %s\n", ToString(aLevel), aMessage.c_str()); });

    std::string error;
    if (!GnsTransport::InitLibrary(error))
    {
        std::fprintf(stderr, "network init failed: %s\n", error.c_str());
        return 1;
    }
    GnsTransport::ApplyImpairment(options.impair);

    const int result = options.mode == "host" ? RunHost(options) : RunClients(options);
    GnsTransport::ShutdownLibrary();
    return result;
}
