using ICities;
using CitiesHarmony.API;

namespace VehicleTelemetryMod
{
    public sealed class Mod : IUserMod
    {
        public string Name
        {
            get { return "Vehicle Telemetry Sender"; }
        }

        public string Description
        {
            get { return "Uploads telemetry and applies server train dispatch permissions."; }
        }

        public void OnEnabled()
        {
            ModLog.Write("Mod.OnEnabled");
            // CitiesHarmony 统一向所有 CS1 Mod 提供同一份 Harmony，避免各 Mod 私带
            // 不同 0Harmony.dll 引起程序集版本冲突。
            HarmonyHelper.DoOnHarmonyReady(HarmonyPatcher.PatchAll);
        }

        public void OnDisabled()
        {
            ModLog.Write("Mod.OnDisabled");
            if (HarmonyHelper.IsHarmonyInstalled)
            {
                HarmonyPatcher.UnpatchAll();
            }
            else
            {
                TrainMotionController.Reset();
            }
        }
    }
}
