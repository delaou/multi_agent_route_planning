using System;
using System.Collections.Generic;
using UnityEngine;

namespace VehicleTelemetryMod
{
    /// <summary>
    /// 游戏端调度执行状态机。
    ///
    /// HTTP 轮询线程只把服务器消息放入 PendingCommands；所有 Vehicle 数据的读取和
    /// 修改都在 ThreadingExtension.OnUpdate 所在的游戏线程完成，避免后台线程与游戏
    /// 仿真线程同时访问 VehicleManager。
    /// </summary>
    internal static class DispatchRuntime
    {
        // 服务器在 1000 m 候选范围内规划；游戏端在更靠近入口的位置实际停车，既给
        // HTTP 往返留出余量，也避免列车在很远处无谓制动。
        private const float HoldDistanceMeters = 300.0f;
        // 当前只跟踪列车头部位置。车头越过出口后再等待 2 秒才上报 cleared，给车尾
        // 离开受控段留出保守余量；后续可用编组长度替换这个近似值。
        private const long TailClearanceDelayMs = 2000;

        private static readonly object TopologyLock = new object();
        private static readonly object CommandsLock = new object();
        private static readonly Queue<DispatchCommandDto> PendingCommands =
            new Queue<DispatchCommandDto>();
        private static readonly Dictionary<string, PassageState> PassagesByTrain =
            new Dictionary<string, PassageState>();
        private static readonly HashSet<string> FinishedCommandIds =
            new HashSet<string>();

        private static List<TopologyCorridorDto> corridors =
            new List<TopologyCorridorDto>();
        private static long topologyRevision;
        private static TrainSnapshot latestSnapshot;
        private static bool tickErrorLogged;

        private sealed class PassageState
        {
            public DispatchCommandDto Command;
            public bool EntryAcknowledged;
            public bool WasInside;
            public long ExitObservedAtMs;
        }

        private sealed class Gate
        {
            public string ResourceId;
            public int StartIndex;
            public int EndIndex;
            public int EntryIndex;
            public float StartPosition;
            public float EndPosition;
            public float EntryPosition;
        }

        /// <summary>清理关卡状态；读档、退出地图时调用。</summary>
        public static void Reset()
        {
            lock (CommandsLock) PendingCommands.Clear();
            PassagesByTrain.Clear();
            FinishedCommandIds.Clear();
            latestSnapshot = null;
            tickErrorLogged = false;
            TrainMotionController.Reset();
            lock (TopologyLock)
            {
                corridors = new List<TopologyCorridorDto>();
                topologyRevision = 0;
            }
        }

        /// <summary>
        /// 在轮询线程解析命令。解析失败只记录日志，不让轮询线程退出。
        /// 同一 command_id 可能在 ACK 前反复出现，真正的去重在游戏线程完成。
        /// </summary>
        public static void AcceptCommandResponse(string json)
        {
            DispatchCommandResponseDto response =
                JsonUtility.FromJson<DispatchCommandResponseDto>(json);
            if (response == null || !response.ok || response.commands == null)
            {
                return;
            }

            for (int i = 0; i < response.commands.Count; ++i)
            {
                if (response.commands[i] != null)
                {
                    lock (CommandsLock) PendingCommands.Enqueue(response.commands[i]);
                }
            }
        }

        /// <summary>
        /// 原子替换服务器分析出的共线段拓扑。拓扑版本变化时旧 RELEASE 不能继续使用，
        /// 因为它的入口/出口索引可能已经指向另一段线路。
        /// </summary>
        public static void AcceptTopologyResponse(string json)
        {
            TopologyResponseDto response =
                JsonUtility.FromJson<TopologyResponseDto>(json);
            if (response == null || !response.ok)
            {
                return;
            }

            lock (TopologyLock)
            {
                if (topologyRevision != 0 && topologyRevision != response.topology_revision)
                {
                    // OnUpdate 会发现活动命令版本不匹配并拒绝它。这里仅换拓扑快照，
                    // 不从网络线程修改 PassagesByTrain。
                    ModLog.Write("Topology revision changed " + topologyRevision + " -> "
                                 + response.topology_revision);
                }
                topologyRevision = response.topology_revision;
                corridors = response.corridors ?? new List<TopologyCorridorDto>();
            }
        }

        /// <summary>保存最近一批列车状态，供逐帧执行器确定线路和行驶方向。</summary>
        public static void UpdateTrainSnapshot(TrainSnapshot snapshot)
        {
            latestSnapshot = snapshot;
        }

        /// <summary>
        /// 每个游戏更新帧执行一次：接收 RELEASE、在入口实施 HOLD、确认进入并在离开后
        /// 释放服务器资源。RELEASE 的含义是“不再施加本地 HOLD”，由游戏原生 TrainAI
        /// 继续驱动列车，而不是把列车瞬移到规划坐标。
        /// </summary>
        public static void Tick()
        {
            TrainMotionController.BeginFrame();
            try
            {
                TickCore();
                tickErrorLogged = false;
            }
            catch (Exception exception)
            {
                // 本帧构造失败时 finally 仍会发布空 HOLD 集合，避免异常让列车永久锁死。
                if (!tickErrorLogged)
                {
                    tickErrorLogged = true;
                    ModLog.Write("DispatchRuntime.Tick failed: " + exception);
                    Debug.LogError("[VehicleTelemetry] Dispatch runtime failed: "
                                   + exception.Message);
                }
            }
            finally
            {
                TrainMotionController.CommitFrame();
            }
        }

        private static void TickCore()
        {
            DrainCommands();
            TrainSnapshot snapshot = latestSnapshot;
            if (snapshot == null || snapshot.trains == null)
            {
                return;
            }

            long currentTopologyRevision;
            List<TopologyCorridorDto> topology;
            lock (TopologyLock)
            {
                currentTopologyRevision = topologyRevision;
                topology = new List<TopologyCorridorDto>(corridors);
            }

            HashSet<string> seenTrains = new HashSet<string>();
            for (int i = 0; i < snapshot.trains.Count; ++i)
            {
                TrainDto train = snapshot.trains[i];
                if (train == null || String.IsNullOrEmpty(train.train_id)) continue;
                seenTrains.Add(train.train_id);

                ushort vehicleId;
                if (!TryParseVehicleId(train.train_id, out vehicleId)) continue;
                Vector3 currentPosition;
                if (!TrainMotionController.TryGetPosition(vehicleId, out currentPosition))
                {
                    continue;
                }

                float routePosition = train.route_position;
                int routeIndex = Mathf.FloorToInt(routePosition);
                PassageState passage;
                PassagesByTrain.TryGetValue(train.train_id, out passage);

                if (passage != null &&
                    ((currentTopologyRevision != 0 &&
                      passage.Command.topology_revision != currentTopologyRevision) ||
                     passage.Command.line_id != train.line_id ||
                     !passage.Command.has_route_bounds))
                {
                    RejectAndFinish(passage, snapshot.simulation_time_ms,
                                    "topology changed or route bounds missing");
                    PassagesByTrain.Remove(train.train_id);
                    passage = null;
                }

                if (passage != null)
                {
                    UpdatePassage(train, routePosition, snapshot.simulation_time_ms, passage);
                    // 获得 RELEASE 的列车不再被当前资源的入口门控停车；若它还接近另一个
                    // 共线段，下面仍会为那个不同资源建立独立门控。
                }

                bool shouldHold = false;
                List<Gate> gates = BuildGates(topology, train);
                for (int gateIndex = 0; gateIndex < gates.Count; ++gateIndex)
                {
                    Gate gate = gates[gateIndex];
                    if (passage != null &&
                        passage.Command.corridor_resource_id == gate.ResourceId)
                    {
                        continue;
                    }
                    if (IsInside(routePosition, gate.StartPosition, gate.EndPosition))
                    {
                        // 不能把一列已在共线段内的列车突然刹停；这种异常由服务器状态和
                        // 日志暴露，入口硬约束只对尚未进入的列车生效。
                        continue;
                    }
                    if (IsApproaching(train.travel_direction, routePosition, gate) &&
                        IsNearEntry(train.line_id, routePosition, gate.EntryPosition,
                                    train.travel_direction, HoldDistanceMeters))
                    {
                        shouldHold = true;
                        break;
                    }
                }

                if (shouldHold)
                {
                    TrainMotionController.HoldTrain(vehicleId);
                }
            }

            RemoveMissingPassages(seenTrains, snapshot.simulation_time_ms);
        }

        private static bool TryDequeueCommand(out DispatchCommandDto command)
        {
            lock (CommandsLock)
            {
                command = PendingCommands.Count == 0 ? null : PendingCommands.Dequeue();
                return command != null;
            }
        }

        private static void DrainCommands()
        {
            DispatchCommandDto command;
            while (TryDequeueCommand(out command))
            {
                if (command == null || command.action != "release" ||
                    String.IsNullOrEmpty(command.train_id) ||
                    String.IsNullOrEmpty(command.corridor_resource_id))
                {
                    continue;
                }

                string identity = CommandIdentity(command);
                if (FinishedCommandIds.Contains(identity)) continue;

                PassageState existing;
                if (PassagesByTrain.TryGetValue(command.train_id, out existing))
                {
                    if (CommandIdentity(existing.Command) == identity) continue;
                    RejectAndFinish(existing, CurrentSimulationTimeMs(),
                                    "superseded by newer command");
                }

                PassageState state = new PassageState();
                state.Command = command;
                PassagesByTrain[command.train_id] = state;
                ModLog.Write("Accepted local RELEASE token command=" + identity
                             + " train=" + command.train_id
                             + " resource=" + command.corridor_resource_id);
            }
        }

        private static void UpdatePassage(TrainDto train, float routePosition,
                                          long simulationTimeMs,
                                          PassageState passage)
        {
            float start = Math.Min(passage.Command.entry_route_position,
                                   passage.Command.exit_route_position);
            float end = Math.Max(passage.Command.entry_route_position,
                                 passage.Command.exit_route_position);
            bool inside = IsInside(routePosition, start, end);

            if (inside)
            {
                passage.WasInside = true;
                passage.ExitObservedAtMs = 0;
                if (!passage.EntryAcknowledged)
                {
                    passage.EntryAcknowledged = true;
                    TelemetrySender.EnqueueDispatchAck(
                        passage.Command.command_id, train.train_id,
                        passage.Command.corridor_resource_id, true,
                        simulationTimeMs);
                }
                return;
            }

            if (!passage.EntryAcknowledged || !passage.WasInside) return;
            if (passage.ExitObservedAtMs == 0)
            {
                passage.ExitObservedAtMs = simulationTimeMs;
                return;
            }
            if (simulationTimeMs - passage.ExitObservedAtMs < TailClearanceDelayMs) return;

            TelemetrySender.EnqueueResourceCleared(
                train.train_id, passage.Command.corridor_resource_id,
                simulationTimeMs);
            Finish(passage);
            PassagesByTrain.Remove(train.train_id);
        }

        private static List<Gate> BuildGates(List<TopologyCorridorDto> topology,
                                             TrainDto train)
        {
            List<Gate> result = new List<Gate>();
            bool routeForward = train.travel_direction != "reverse";
            for (int corridorIndex = 0; corridorIndex < topology.Count; ++corridorIndex)
            {
                TopologyCorridorDto corridor = topology[corridorIndex];
                if (corridor == null || corridor.members == null) continue;
                for (int memberIndex = 0; memberIndex < corridor.members.Count; ++memberIndex)
                {
                    TopologyCorridorMemberDto member = corridor.members[memberIndex];
                    if (member == null || member.line_id != train.line_id) continue;

                    int start = Math.Min(member.start_segment, member.end_segment);
                    int end = Math.Max(member.start_segment, member.end_segment);
                    bool relationSame = member.direction_relation != "opposite";
                    bool resourceForward = routeForward == relationSame;
                    Gate gate = new Gate();
                    gate.ResourceId = corridor.corridor_id +
                        (resourceForward ? ":FORWARD" : ":REVERSE");
                    gate.StartIndex = start;
                    gate.EndIndex = end;
                    gate.EntryIndex = routeForward ? start : end;
                    gate.StartPosition = Math.Min(member.start_position, member.end_position);
                    gate.EndPosition = Math.Max(member.start_position, member.end_position);
                    gate.EntryPosition = routeForward ? gate.StartPosition : gate.EndPosition;
                    result.Add(gate);
                }
            }
            return result;
        }

        private static bool IsApproaching(string travelDirection, float routePosition, Gate gate)
        {
            return travelDirection == "reverse"
                ? routePosition > gate.EndPosition
                : routePosition < gate.StartPosition;
        }

        private static bool IsNearEntry(string lineId, float routePosition, float entryPosition,
                                        string travelDirection, float maximumDistance)
        {
            float distance;
            return LineRouteCollector.TryGetDistanceAlongRoute(
                       lineId, routePosition, entryPosition,
                       travelDirection != "reverse", out distance) &&
                   distance <= maximumDistance;
        }

        private static bool IsInside(float routePosition, float start, float end)
        {
            return routePosition >= start && routePosition <= end;
        }

        private static void RejectAndFinish(PassageState passage, long simulationTimeMs,
                                            string reason)
        {
            if (!passage.EntryAcknowledged)
            {
                TelemetrySender.EnqueueDispatchAck(
                    passage.Command.command_id, passage.Command.train_id,
                    passage.Command.corridor_resource_id, false, simulationTimeMs);
            }
            ModLog.Write("Rejected RELEASE command=" + CommandIdentity(passage.Command)
                         + " reason=" + reason);
            Finish(passage);
        }

        private static void Finish(PassageState passage)
        {
            FinishedCommandIds.Add(CommandIdentity(passage.Command));
            // 防止长时间游玩后集合无限增长。命令 ID 单调递增，清空只会造成极低概率的
            // 旧响应再处理；服务器在 ACK 后已不再返回该命令。
            if (FinishedCommandIds.Count > 4096) FinishedCommandIds.Clear();
        }

        private static void RemoveMissingPassages(HashSet<string> seenTrains,
                                                  long simulationTimeMs)
        {
            List<string> missing = new List<string>();
            foreach (KeyValuePair<string, PassageState> pair in PassagesByTrain)
            {
                if (!seenTrains.Contains(pair.Key)) missing.Add(pair.Key);
            }
            for (int i = 0; i < missing.Count; ++i)
            {
                PassageState passage = PassagesByTrain[missing[i]];
                if (!passage.EntryAcknowledged)
                {
                    RejectAndFinish(passage, simulationTimeMs, "train disappeared");
                }
                else
                {
                    // 车辆被游戏删除时服务器也必须释放占用，否则资源会永久锁死。
                    TelemetrySender.EnqueueResourceCleared(
                        passage.Command.train_id,
                        passage.Command.corridor_resource_id,
                        simulationTimeMs);
                    Finish(passage);
                }
                PassagesByTrain.Remove(missing[i]);
            }
        }

        private static string CommandIdentity(DispatchCommandDto command)
        {
            if (!String.IsNullOrEmpty(command.command_id)) return command.command_id;
            return command.train_id + "|" + command.corridor_resource_id + "|"
                   + command.issued_at_simulation_ms;
        }

        private static bool TryParseVehicleId(string trainId, out ushort vehicleId)
        {
            vehicleId = 0;
            const string prefix = "train_";
            return trainId.StartsWith(prefix, StringComparison.Ordinal) &&
                   UInt16.TryParse(trainId.Substring(prefix.Length), out vehicleId) &&
                   vehicleId != 0;
        }

        private static long CurrentSimulationTimeMs()
        {
            SimulationManager manager = SimulationManager.instance;
            double frameMs = manager.m_timePerFrame.TotalMilliseconds;
            if (frameMs <= 0.0) frameMs = 1000.0 / 60.0;
            return (long)(manager.m_currentFrameIndex * frameMs);
        }
    }
}
