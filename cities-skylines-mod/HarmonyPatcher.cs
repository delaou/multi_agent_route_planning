using System;
using System.Collections.Generic;
using System.Reflection;
using HarmonyLib;
using UnityEngine;

namespace VehicleTelemetryMod
{
    /// <summary>集中管理本 Mod 的 Harmony 补丁，确保重复启用和卸载都是幂等的。</summary>
    internal static class HarmonyPatcher
    {
        // 全局唯一 ID 使 Harmony 只卸载本 Mod 的补丁，不影响 TM:PE 等其他 Mod。
        public const string HarmonyId = "com.vehicletelemetry.dispatch.train-speed-gate";
        private static bool patched;

        public static void PatchAll()
        {
            if (patched) return;
            try
            {
                List<MethodBase> targets = new List<MethodBase>(
                    TrainAiSpeedPatch.FindTargetMethods());
                if (targets.Count == 0)
                {
                    throw new MissingMethodException(
                        "No compatible TrainAI.CalculateSegmentPosition overload found");
                }

                Harmony harmony = new Harmony(HarmonyId);
                harmony.PatchAll(typeof(HarmonyPatcher).Assembly);
                patched = true;
                ModLog.Write("Harmony train speed gate installed targets=" + targets.Count);
            }
            catch (Exception exception)
            {
                patched = false;
                TrainMotionController.Reset();
                ModLog.Write("Harmony patch installation failed: " + exception);
                Debug.LogError("[VehicleTelemetry] Train dispatch patch failed: "
                               + exception.Message);
            }
        }

        public static void UnpatchAll()
        {
            TrainMotionController.Reset();
            if (!patched) return;
            try
            {
                new Harmony(HarmonyId).UnpatchAll(HarmonyId);
                ModLog.Write("Harmony train speed gate removed");
            }
            catch (Exception exception)
            {
                ModLog.Write("Harmony patch removal failed: " + exception);
                Debug.LogError("[VehicleTelemetry] Train dispatch unpatch failed: "
                               + exception.Message);
            }
            finally
            {
                patched = false;
            }
        }
    }

    /// <summary>
    /// 在 TrainAI 完成原生速度计算后施加额外上限。使用 Postfix + Priority.Last，
    /// 可保留原生信号、车站、跟车逻辑以及其他 Mod 先前给出的更低速度。
    /// </summary>
    [HarmonyPatch]
    internal static class TrainAiSpeedPatch
    {
        /// <summary>
        /// 不把完整参数类型写死，而是在运行时寻找 CS1 的标准 8 参数重载：首参数为
        /// vehicleID、第二参数为 ref Vehicle、最后参数为 out float maxSpeed。
        /// 这样可以兼容方法可见性变化，同时拒绝结构未知的重载。
        /// </summary>
        internal static IEnumerable<MethodBase> FindTargetMethods()
        {
            MethodInfo[] methods = typeof(TrainAI).GetMethods(
                BindingFlags.Instance | BindingFlags.Public |
                BindingFlags.NonPublic | BindingFlags.DeclaredOnly);
            for (int i = 0; i < methods.Length; ++i)
            {
                MethodInfo method = methods[i];
                if (method.Name != "CalculateSegmentPosition") continue;
                ParameterInfo[] parameters = method.GetParameters();
                if (parameters.Length != 8 ||
                    parameters[0].ParameterType != typeof(ushort) ||
                    parameters[1].ParameterType != typeof(Vehicle).MakeByRefType() ||
                    parameters[7].ParameterType != typeof(float).MakeByRefType())
                {
                    continue;
                }
                yield return method;
            }
        }

        // Harmony 约定：TargetMethods 返回要应用当前 Patch 类的全部原方法。
        private static IEnumerable<MethodBase> TargetMethods()
        {
            return FindTargetMethods();
        }

        [HarmonyPostfix]
        [HarmonyPriority(Priority.Last)]
        private static void Postfix(ushort __0, ref float __7)
        {
            if (TrainMotionController.ShouldHold(__0))
            {
                // 只降低游戏计算出的速度，绝不主动提高速度或强制越过原生信号。
                __7 = 0.0f;
            }
        }
    }
}
