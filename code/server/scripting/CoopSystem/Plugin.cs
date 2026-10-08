using Cyberpunk.Rpc.Client.Plugins;
using CyberpunkSdk;
using CyberpunkSdk.Types;

namespace CoopSystem
{
    // Seamless story co-op: one player is the story host, the others follow the host's world.
    //
    // Everyone plays from their own story save. The first player to connect becomes the story host; when the host
    // leaves, the longest-connected player takes over, and "/makehost" hands it to whoever types it. The host's game
    // reports its world every few seconds (CoopServer.ReportWorld: game time, weather, tracked quest), which is
    // relayed to every guest (CoopClient.ApplyWorld). Chat commands starting with "/" arrive here (CoopServer.Command).
    // Client side: code/assets/redscript/Plugins/Coop.reds.
    public class Plugin
    {
        public static Plugin Instance { get; }

        static Plugin()
        {
            Instance = new Plugin();
        }

        private const string ChatName = "Co-op";

        private readonly object _lock = new();
        private readonly List<ulong> _players = new(); // in the order they joined
        private ulong? _host;

        // The host's last reported world.
        private int _gameTime = -1;
        private CName _weather = new(0);
        private string _quest = "";
        private bool _hasWorld;

        private Plugin()
        {
            Server.PlayerSystem.PlayerJoinEvent += OnPlayerJoin;
            Server.PlayerSystem.PlayerLeftEvent += OnPlayerLeft;
        }

        private static string NameOf(ulong id)
        {
            try
            {
                return Server.PlayerSystem.GetById(id).Username;
            }
            catch
            {
                return $"player {id}";
            }
        }

        private static void Say(ulong id, string message)
        {
            try
            {
                Server.PlayerSystem.GetById(id).SendChat(ChatName, message);
            }
            catch
            {
                // The player left in the meantime.
            }
        }

        private void SayToAll(string message)
        {
            foreach (var id in _players)
                Say(id, message);
        }

        // Tells every player who the story host is.
        private void SendRoles()
        {
            var hostName = _host is ulong host ? NameOf(host) : "";
            foreach (var id in _players)
                CoopClient.SetRole(id, id == _host, hostName);
        }

        private void SendWorldTo(ulong id)
        {
            if (_hasWorld && id != _host)
                CoopClient.ApplyWorld(id, _gameTime, _weather, _quest);
        }

        private void OnPlayerJoin(ulong id)
        {
            lock (_lock)
            {
                _players.Add(id);
                var name = NameOf(id);
                if (_host is null)
                {
                    _host = id;
                    _hasWorld = false;
                }
                CoopClient.SetRole(id, id == _host, NameOf(_host!.Value));
                SendWorldTo(id);
                SayToAll(id == _host
                    ? $"{name} joined and is the story host. Type /help for co-op commands."
                    : $"{name} joined. Story host: {NameOf(_host.Value)}. /tp takes you to them.");
            }
        }

        private void OnPlayerLeft(ulong id)
        {
            lock (_lock)
            {
                var name = NameOf(id);
                _players.Remove(id);
                if (_host != id)
                {
                    SayToAll($"{name} left.");
                    return;
                }
                _host = _players.Count > 0 ? _players[0] : null;
                _hasWorld = false;
                if (_host is ulong host)
                {
                    SendRoles();
                    SayToAll($"{name} (the story host) left. {NameOf(host)} is the story host now.");
                }
            }
        }

        internal void ReportWorld(ulong id, int gameTime, CName weather, string quest)
        {
            lock (_lock)
            {
                if (id != _host)
                    return; // only the host's world counts
                _gameTime = gameTime;
                _weather = weather;
                _quest = quest;
                _hasWorld = true;
                foreach (var player in _players)
                    SendWorldTo(player);
            }
        }

        internal void Command(ulong id, string text)
        {
            var parts = text.Trim().Split(' ', 2, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            var command = parts.Length > 0 ? parts[0].ToLowerInvariant() : "";
            var argument = parts.Length > 1 ? parts[1] : "";

            lock (_lock)
            {
                switch (command)
                {
                case "/help":
                    Say(id, "/host: who the story host is and their quest. /players: who is here. "
                            + "/tp [name]: go to the host (or a player). /sync: follow the host's time and weather now. "
                            + "/makehost: become the story host.");
                    break;

                case "/host":
                    if (_host is ulong host)
                    {
                        var quest = _quest.Length > 0 ? $" Tracked quest: {_quest}." : "";
                        Say(id, $"Story host: {NameOf(host)}{(host == id ? " (you)" : "")}.{quest}");
                    }
                    break;

                case "/players":
                    Say(id, "Players: " + string.Join(", ",
                        _players.Select(p => NameOf(p) + (p == _host ? " (host)" : "") + (p == id ? " (you)" : ""))));
                    break;

                case "/tp":
                    Teleport(id, argument);
                    break;

                case "/sync":
                    if (id == _host)
                        Say(id, "You are the story host: the others follow you.");
                    else if (!_hasWorld)
                        Say(id, "The host's world hasn't arrived yet.");
                    else
                    {
                        SendWorldTo(id);
                        Say(id, "Following the host's time and weather.");
                    }
                    break;

                case "/makehost":
                    if (_host == id)
                    {
                        Say(id, "You already are the story host.");
                        break;
                    }
                    _host = id;
                    _hasWorld = false;
                    SendRoles();
                    SayToAll($"{NameOf(id)} is the story host now.");
                    break;

                default:
                    Say(id, $"Unknown command {command}. Type /help.");
                    break;
                }
            }
        }

        private void Teleport(ulong id, string targetName)
        {
            ulong? target = _host;
            if (targetName.Length > 0)
            {
                target = _players.FirstOrDefault(p =>
                    NameOf(p).StartsWith(targetName, StringComparison.OrdinalIgnoreCase));
                if (target == 0)
                {
                    Say(id, $"No player called {targetName}. /players lists them.");
                    return;
                }
            }
            if (target is not ulong to || to == id)
            {
                Say(id, "That's you.");
                return;
            }
            var position = Server.PlayerSystem.GetById(to).MovementComponent.Position;
            if (position.X == 0.0f && position.Y == 0.0f && position.Z == 0.0f)
            {
                Say(id, $"{NameOf(to)} isn't placed in the world yet.");
                return;
            }
            // A step beside them, not inside them.
            CoopClient.TeleportTo(id, position.X + 1.0f, position.Y, position.Z);
            Say(id, $"Going to {NameOf(to)}.");
        }
    }
}
