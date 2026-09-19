using System;
using System.Collections.Generic;
using UnityEngine;

namespace VehicleTelemetryMod
{
    internal static class TrainSnapshotCollector
    {
        private static readonly Dictionary<string, float> LastRouteIndexByTrain =
            new Dictionary<string, float>();
        private static readonly Dictionary<string, string> LastDirectionByTrain =
            new Dictionary<string, string>();
        private static readonly Dictionary<string, float> LastRoutePositionByTrain =
            new Dictionary<string, float>();

        public static TrainSnapshot Capture()
        {
            List<TrainDto> trains = new List<TrainDto>();
            HashSet<string> seen = new HashSet<string>();
            TransportManager manager = TransportManager.instance;
            TransportLine[] lines = manager.m_lines.m_buffer;

            for (int index = 1; index < lines.Length; ++index)
            {
                ushort lineId = (ushort)index;
                TransportLine line = lines[lineId];
                if ((line.m_flags & TransportLine.Flags.Created) == 0)
                {
                    continue;
                }

                TransportInfo info = line.Info;
                if (!LineRouteCollector.IsRailTransport(info))
                {
                    continue;
                }

                string routeId = LineRouteCollector.RouteId(lineId, info);
                int vehicleCount = line.CountVehicles(lineId);
                for (int vehicleIndex = 0; vehicleIndex < vehicleCount; ++vehicleIndex)
                {
                    ushort vehicleId = line.GetVehicle(vehicleIndex);
                    if (vehicleId == 0)
                    {
                        continue;
                    }

                    Vehicle vehicle = VehicleManager.instance.m_vehicles.m_buffer[vehicleId];
                    if ((vehicle.m_flags & Vehicle.Flags.Created) == 0)
                    {
                        continue;
                    }

                    Vector3 position = vehicle.GetLastFramePosition();
                    Vector3 velocity = vehicle.GetLastFrameVelocity();
                    string trainId = "train_" + vehicleId.ToString();
                    float previousPosition;
                    bool hasPrevious = LastRoutePositionByTrain.TryGetValue(
                        trainId, out previousPosition);
                    float routePosition = LineRouteCollector.FindRoutePosition(
                        routeId, position, velocity, previousPosition, hasPrevious);
                    int routeIndex = Mathf.FloorToInt(routePosition);

                    TrainDto dto = new TrainDto();
                    dto.train_id = trainId;
                    dto.line_id = routeId;
                    dto.travel_direction = InferTravelDirection(
                        trainId, routeId, routePosition);
                    dto.status = vehicle.m_flags.ToString();
                    dto.position = new TrainPositionDto();
                    dto.position.x = position.x;
                    dto.position.y = position.y;
                    dto.position.z = position.z;
                    dto.speed = velocity.magnitude;
                    dto.route_index = routeIndex;
                    dto.route_position = routePosition;
                    LastRoutePositionByTrain[trainId] = routePosition;
                    trains.Add(dto);
                    seen.Add(trainId);
                }
            }

            RemoveMissingTrains(seen);

            TrainSnapshot snapshot = new TrainSnapshot();
            snapshot.simulation_time_ms = SimulationTimeMilliseconds();
            snapshot.trains = trains;
            return snapshot;
        }

        private static string InferTravelDirection(string trainId, string lineId,
                                                   float routePosition)
        {
            float previous;
            string direction;
            if (!LastDirectionByTrain.TryGetValue(trainId, out direction))
            {
                direction = "forward";
            }
            if (LastRouteIndexByTrain.TryGetValue(trainId, out previous))
            {
                float delta = routePosition - previous;
                int pointCount = LineRouteCollector.GetRoutePointCount(lineId);
                // 环线从末尾跳到 0 仍是 forward，从 0 跳到末尾仍是 reverse。
                // 索引相同通常只是低速或采样粒度造成，保留上一帧方向而不抖动。
                if (LineRouteCollector.IsLoop(lineId) && pointCount > 2 &&
                    Math.Abs(delta) > pointCount / 2.0f)
                {
                    delta = -delta;
                }
                if (delta < 0) direction = "reverse";
                else if (delta > 0) direction = "forward";
            }

            LastRouteIndexByTrain[trainId] = routePosition;
            LastDirectionByTrain[trainId] = direction;
            return direction;
        }

        private static void RemoveMissingTrains(HashSet<string> seen)
        {
            List<string> missing = new List<string>();
            foreach (KeyValuePair<string, float> pair in LastRouteIndexByTrain)
            {
                if (!seen.Contains(pair.Key))
                {
                    missing.Add(pair.Key);
                }
            }

            for (int i = 0; i < missing.Count; ++i)
            {
                LastRouteIndexByTrain.Remove(missing[i]);
                LastDirectionByTrain.Remove(missing[i]);
                LastRoutePositionByTrain.Remove(missing[i]);
            }
        }

        private static long SimulationTimeMilliseconds()
        {
            SimulationManager manager = SimulationManager.instance;
            double frameMs = manager.m_timePerFrame.TotalMilliseconds;
            if (frameMs <= 0.0)
            {
                frameMs = 1000.0 / 60.0;
            }
            return (long)(manager.m_currentFrameIndex * frameMs);
        }
    }
}
