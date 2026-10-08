#pragma once

// A whole session in one process, deterministic: in-memory transport, manual clock, stepped at 60 frames per
// second. The host's own player is machines[0]; every player is a scripted SimPlayer.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "MemoryTransport.hpp"
#include "client/ClientSession.hpp"
#include "host/HostService.hpp"
#include "tools/sim/SimPlayer.hpp"

namespace test
{
using namespace coop;

constexpr uint16_t kPort = 1;
constexpr TimeUs kFrameUs = 16'667;

struct Machine
{
    std::unique_ptr<test::MemoryTransport> transport;
    std::unique_ptr<sim::SimPlayer> sim;
    std::unique_ptr<ClientSession> session;

    [[nodiscard]] PeerId Peer() const { return session->LocalPeer(); }
};

struct World
{
    ManualClock clock;
    test::MemoryNetwork network;
    std::unique_ptr<test::MemoryTransport> hostTransport;
    std::unique_ptr<HostService> host;
    std::vector<std::unique_ptr<Machine>> machines; // [0] is the host's own player

    explicit World(const std::function<void(HostConfig&)>& aConfigure = {})
    {
        clock.Set(1'000'000);
        HostConfig config;
        config.port = kPort;
        config.gameBuild = "test";
        config.passwordIterations = 1000;
        if (aConfigure)
            aConfigure(config);
        hostTransport = std::make_unique<test::MemoryTransport>(network);
        host = std::make_unique<HostService>(*hostTransport, clock, config);
        std::string error;
        host->Start(error);

        auto& player = MakeMachine("Host", sim::Script::Idle, Vec3{0.0f, 0.0f, 0.0f});
        ConnId hostSide = kInvalidConn;
        ConnId playerSide = kInvalidConn;
        test::MemoryTransport::CreatePair(*hostTransport, hostSide, *player.transport, playerSide);
        host->MarkLocal(hostSide);
        host->AdoptConnection(hostSide);
        player.session->UseConnection(playerSide);
    }

    ~World()
    {
        for (auto& machine : machines)
            machine->session.reset();
        host.reset();
    }

    Machine& MakeMachine(const std::string& aName, sim::Script aScript, Vec3 aCenter)
    {
        auto machine = std::make_unique<Machine>();
        machine->transport = std::make_unique<test::MemoryTransport>(network);
        sim::SimPlayerConfig config;
        config.name = aName;
        config.script = aScript;
        config.center = aCenter;
        config.radius = 20.0f;
        config.verbose = false;
        machine->sim = std::make_unique<sim::SimPlayer>(clock, config);
        ClientConfig client;
        client.displayName = aName;
        client.gameBuild = "test";
        client.clientId[0] = static_cast<uint8_t>(machines.size() + 1);
        machine->session = std::make_unique<ClientSession>(*machine->transport, *machine->sim, clock, client);
        machines.push_back(std::move(machine));
        return *machines.back();
    }

    Machine& AddClient(const std::string& aName, sim::Script aScript, Vec3 aCenter)
    {
        auto& machine = MakeMachine(aName, aScript, aCenter);
        std::string error;
        machine.session->Connect("mem:" + std::to_string(kPort), error);
        return machine;
    }

    Machine& HostPlayer() { return *machines[0]; }

    // Optional hook run after each machine's tick (e.g. to record per-frame values).
    std::function<void(Machine&)> onFrame;

    void Step(int aFrames = 1)
    {
        for (int frame = 0; frame < aFrames; ++frame)
        {
            clock.Advance(kFrameUs);
            host->Tick();
            for (auto& machine : machines)
            {
                if (!machine->session)
                    continue;
                machine->session->Tick();
                sim::PumpCommands(*machine->sim, *machine->session);
                if (onFrame)
                    onFrame(*machine);
            }
        }
    }

    template<typename Predicate>
    bool RunUntil(Predicate aDone, int aMaxFrames = 900)
    {
        for (int frame = 0; frame < aMaxFrames; ++frame)
        {
            Step();
            if (aDone())
                return true;
        }
        return false;
    }

    // Everyone joined and synced.
    bool Ready()
    {
        for (auto& machine : machines)
        {
            if (machine->session->State() != ClientState::Joined || !machine->session->HasSessionTime())
                return false;
        }
        return true;
    }
};

} // namespace test
