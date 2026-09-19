using System.Collections.Generic;
using System.Threading;
using UnityEngine;

namespace VehicleTelemetryMod
{
    /// <summary>
    /// Harmony 补丁与调度状态机之间的轻量状态桥梁。
    ///
    /// DispatchRuntime 在一帧内构造“需要停车的车辆 ID”集合，随后一次性发布；
    /// TrainAI 的 Harmony Postfix 只执行无锁查询。发布后的 HashSet 不再修改，因此
    /// 即使补丁与 OnUpdate 不在同一线程，也不会读到构造一半的集合。
    /// </summary>
    internal static class TrainMotionController
    {
        private const int MaximumConsistVehicles = 256;
        private static HashSet<ushort> activeHeldVehicles = new HashSet<ushort>();
        private static HashSet<ushort> buildingHeldVehicles = new HashSet<ushort>();

        /// <summary>开始构造新一帧 HOLD 快照。</summary>
        public static void BeginFrame()
        {
            buildingHeldVehicles.Clear();
        }

        /// <summary>
        /// 把一列车的整套编组加入 HOLD，而不是只限制服务器 train_id 对应的头车。
        /// 输入 vehicleId 即使恰好是中间车，也会先沿 m_leadingVehicle 找到头车，
        /// 再沿 m_trailingVehicle 遍历整列；访问次数设置上限以防损坏存档形成环。
        /// </summary>
        public static void HoldTrain(ushort vehicleId)
        {
            Vehicle[] vehicles = VehicleManager.instance.m_vehicles.m_buffer;
            if (!IsCreated(vehicles, vehicleId)) return;

            ushort head = vehicleId;
            HashSet<ushort> visited = new HashSet<ushort>();
            for (int i = 0; i < MaximumConsistVehicles; ++i)
            {
                if (!visited.Add(head) || !IsCreated(vehicles, head)) break;
                ushort leading = vehicles[head].m_leadingVehicle;
                if (leading == 0 || !IsCreated(vehicles, leading)) break;
                head = leading;
            }

            visited.Clear();
            ushort current = head;
            for (int i = 0; i < MaximumConsistVehicles; ++i)
            {
                if (!IsCreated(vehicles, current) || !visited.Add(current)) break;
                buildingHeldVehicles.Add(current);
                ushort trailing = vehicles[current].m_trailingVehicle;
                if (trailing == 0) break;
                current = trailing;
            }
        }

        /// <summary>
        /// 原子发布本帧结果。没有被重新加入的车辆自然变成 RELEASE；不需要清除
        /// Vehicle.Flags，也不会干扰车站停车、红灯和游戏自身的路径状态。
        /// </summary>
        public static void CommitFrame()
        {
            HashSet<ushort> published = buildingHeldVehicles;
            buildingHeldVehicles = new HashSet<ushort>();
            Interlocked.Exchange(ref activeHeldVehicles, published);
        }

        /// <summary>供 Harmony 热路径查询；只读已发布集合，不进行几何或网络操作。</summary>
        public static bool ShouldHold(ushort vehicleId)
        {
            HashSet<ushort> snapshot =
                Interlocked.CompareExchange(ref activeHeldVehicles, null, null);
            return snapshot.Contains(vehicleId);
        }

        public static bool TryGetPosition(ushort vehicleId, out Vector3 position)
        {
            position = Vector3.zero;
            Vehicle[] vehicles = VehicleManager.instance.m_vehicles.m_buffer;
            if (!IsCreated(vehicles, vehicleId)) return false;
            position = vehicles[vehicleId].GetLastFramePosition();
            return true;
        }

        /// <summary>读档、禁用 Mod 或补丁卸载时立即清空全部调度限速。</summary>
        public static void Reset()
        {
            buildingHeldVehicles.Clear();
            Interlocked.Exchange(ref activeHeldVehicles, new HashSet<ushort>());
        }

        private static bool IsCreated(Vehicle[] vehicles, ushort vehicleId)
        {
            return vehicleId != 0 && vehicleId < vehicles.Length &&
                   (vehicles[vehicleId].m_flags & Vehicle.Flags.Created) != 0;
        }
    }
}
