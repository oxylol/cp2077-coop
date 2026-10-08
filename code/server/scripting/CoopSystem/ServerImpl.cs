using CoopSystem;
using CyberpunkSdk.Types;

namespace Cyberpunk.Rpc.Server.Plugins
{
    public static partial class CoopServer
    {
        public static partial void ReportWorld_Impl(ulong playerId, int gameTime, CName weather, string quest)
        {
            Plugin.Instance.ReportWorld(playerId, gameTime, weather, quest);
        }

        public static partial void Command_Impl(ulong playerId, string text)
        {
            Plugin.Instance.Command(playerId, text);
        }
    }
}
