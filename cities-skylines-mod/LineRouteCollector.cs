using System;
using System.Collections.Generic;
using ColossalFramework.Math;
using UnityEngine;

namespace VehicleTelemetryMod
{
    internal static class LineRouteCollector
    {
        private const float DuplicatePointTolerance = 1.0f;
        private static readonly object SyncRoot = new object();
        private static readonly Dictionary<string, List<LineRoutePointDto>> RoutesByLineId =
            new Dictionary<string, List<LineRoutePointDto>>();

        public static LineRouteSnapshot Capture()
        {
            long now = UnixTimeMilliseconds();
            List<LineRouteDto> routes = new List<LineRouteDto>();
            Dictionary<string, List<LineRoutePointDto>> newRoutes =
                new Dictionary<string, List<LineRoutePointDto>>();

            TransportManager manager = TransportManager.instance;
            TransportLine[] buffer = manager.m_lines.m_buffer;

            for (int index = 1; index < buffer.Length; ++index)
            {
                ushort lineId = (ushort)index;
                TransportLine line = buffer[lineId];
                if ((line.m_flags & TransportLine.Flags.Created) == 0)
                {
                    continue;
                }

                TransportInfo info = line.Info;
                if (info == null || !IsRailTransport(info))
                {
                    continue;
                }

                List<LineRoutePointDto> points = CaptureCurvePoints(
                    manager, lineId, info.m_vehicleType);
                string geometrySource = "curve";
                if (points.Count < 2)
                {
                    points = CaptureStopPoints(lineId, line, info.m_vehicleType);
                    geometrySource = "stops";
                }

                if (points.Count < 2)
                {
                    continue;
                }

                string transportType = NormalizeTransportType(info);
                string routeId = RouteId(lineId, info);
                LineRouteDto route = new LineRouteDto();
                route.line_id = routeId;
                route.line_name = manager.GetLineName(lineId);
                route.direction = "forward";
                route.transport_type = transportType;
                route.geometry_source = geometrySource;
                route.is_loop = IsClosed(points);
                route.version = now;
                route.updated_at = now;
                route.target_headway_ms = 15000;
                route.points = points;
                routes.Add(route);
                newRoutes[routeId] = points;
            }

            lock (SyncRoot)
            {
                RoutesByLineId.Clear();
                foreach (KeyValuePair<string, List<LineRoutePointDto>> pair in newRoutes)
                {
                    RoutesByLineId[pair.Key] = pair.Value;
                }
            }

            LineRouteSnapshot snapshot = new LineRouteSnapshot();
            snapshot.timestamp = now;
            snapshot.version = now;
            snapshot.routes = routes;
            return snapshot;
        }

        public static string RouteId(ushort lineId, TransportInfo info)
        {
            return NormalizeTransportType(info) + "_line_" + lineId.ToString();
        }

        public static bool IsRailTransport(TransportInfo info)
        {
            if (info == null) return false;
            string type = info.m_transportType.ToString();
            return type == "Train" || type == "Metro" || type == "Monorail" ||
                   type == "Tram";
        }

        public static int FindNearestRouteIndex(string lineId, Vector3 position)
        {
            List<LineRoutePointDto> points;
            lock (SyncRoot)
            {
                if (!RoutesByLineId.TryGetValue(lineId, out points) ||
                    points == null || points.Count < 2)
                {
                    return 0;
                }

                // The list is replaced atomically by Capture(), so keeping the
                // reference after the lock is safe for read-only iteration.
            }

            int bestIndex = 0;
            float bestDistanceSq = float.MaxValue;
            for (int i = 0; i + 1 < points.Count; ++i)
            {
                float distanceSq = DistanceToSegmentSq(
                    position.x, position.z,
                    points[i].x, points[i].z,
                    points[i + 1].x, points[i + 1].z);
                if (distanceSq < bestDistanceSq)
                {
                    bestDistanceSq = distanceSq;
                    bestIndex = i;
                }
            }

            return bestIndex;
        }

        // 把车辆投影到折线内部，返回 segment+t。高度、速度方向和上一帧位置共同
        // 消除环线回头处、上下行近邻和同一几何往返段的二义性。
        public static float FindRoutePosition(string lineId, Vector3 position,
                                              Vector3 velocity, float previous,
                                              bool hasPrevious)
        {
            List<LineRoutePointDto> points;
            lock (SyncRoot)
            {
                if (!RoutesByLineId.TryGetValue(lineId, out points) ||
                    points == null || points.Count < 2) return 0.0f;
            }
            float best = 0.0f;
            float bestScore = float.MaxValue;
            float maximum = points.Count - 1;
            for (int i = 0; i + 1 < points.Count; ++i)
            {
                float abx = points[i + 1].x - points[i].x;
                float aby = points[i + 1].y - points[i].y;
                float abz = points[i + 1].z - points[i].z;
                float denominator = abx * abx + abz * abz;
                float t = denominator <= 0.0001f ? 0.0f :
                    ((position.x - points[i].x) * abx +
                     (position.z - points[i].z) * abz) / denominator;
                t = Mathf.Clamp01(t);
                float dx = position.x - (points[i].x + t * abx);
                float dy = position.y - (points[i].y + t * aby);
                float dz = position.z - (points[i].z + t * abz);
                float score = dx * dx + dz * dz + 4.0f * dy * dy;
                float candidate = i + t;
                if (velocity.sqrMagnitude > 0.04f && denominator > 0.0001f)
                {
                    float alignment = (velocity.x * abx + velocity.z * abz) /
                        Mathf.Sqrt((velocity.x * velocity.x + velocity.z * velocity.z) * denominator);
                    score += 9.0f * (1.0f - Mathf.Abs(alignment));
                }
                if (hasPrevious)
                {
                    float continuity = Mathf.Abs(candidate - previous);
                    if (IsClosed(points)) continuity = Mathf.Min(continuity, maximum - continuity);
                    score += 0.25f * continuity * continuity;
                }
                if (score < bestScore) { bestScore = score; best = candidate; }
            }
            return best;
        }

        /// <summary>
        /// 返回线路指定采样索引的三维世界坐标。调度执行器用该坐标估算列车到
        /// 共线段入口的距离；缓存由 Capture() 整体替换，读取期间不会修改列表。
        /// </summary>
        public static bool TryGetRoutePoint(string lineId, int routeIndex, out Vector3 point)
        {
            point = Vector3.zero;
            List<LineRoutePointDto> points;
            lock (SyncRoot)
            {
                if (string.IsNullOrEmpty(lineId) ||
                    !RoutesByLineId.TryGetValue(lineId, out points) ||
                    points == null || routeIndex < 0 || routeIndex >= points.Count)
                {
                    return false;
                }

                LineRoutePointDto routePoint = points[routeIndex];
                point = new Vector3(routePoint.x, routePoint.y, routePoint.z);
                return true;
            }
        }

        /// <summary>返回线路采样点数量，供环线方向判断识别索引首尾跳变。</summary>
        public static int GetRoutePointCount(string lineId)
        {
            List<LineRoutePointDto> points;
            lock (SyncRoot)
            {
                return RoutesByLineId.TryGetValue(lineId, out points) && points != null
                    ? points.Count : 0;
            }
        }

        public static bool IsLoop(string lineId)
        {
            List<LineRoutePointDto> points;
            lock (SyncRoot)
            {
                return RoutesByLineId.TryGetValue(lineId, out points) && IsClosed(points);
            }
        }

        /// <summary>
        /// 计算从当前 segment 到入口 segment 的沿线路水平距离。相比两点直线距离，
        /// 该值不会在回头弯、立交或平行近邻处错误地提前触发停车。
        /// </summary>
        public static bool TryGetDistanceAlongRoute(string lineId,
                                                    int fromSegment,
                                                    int entrySegment,
                                                    bool increasing,
                                                    out float distance)
        {
            distance = 0.0f;
            List<LineRoutePointDto> points;
            lock (SyncRoot)
            {
                if (!RoutesByLineId.TryGetValue(lineId, out points) || points == null ||
                    points.Count < 2 || fromSegment < 0 || entrySegment < 0 ||
                    fromSegment + 1 >= points.Count || entrySegment + 1 >= points.Count)
                {
                    return false;
                }
                if ((increasing && fromSegment > entrySegment) ||
                    (!increasing && fromSegment < entrySegment))
                {
                    return false;
                }

                int low = Math.Min(fromSegment, entrySegment);
                int high = Math.Max(fromSegment, entrySegment);
                for (int i = low; i < high; ++i)
                {
                    float dx = points[i + 1].x - points[i].x;
                    float dz = points[i + 1].z - points[i].z;
                    distance += (float)Math.Sqrt(dx * dx + dz * dz);
                }
                return true;
            }
        }

        public static bool TryGetDistanceAlongRoute(string lineId,
                                                    float fromPosition,
                                                    float entryPosition,
                                                    bool increasing,
                                                    out float distance)
        {
            distance = 0.0f;
            List<LineRoutePointDto> points;
            lock (SyncRoot)
            {
                if (!RoutesByLineId.TryGetValue(lineId, out points) || points == null ||
                    points.Count < 2) return false;
                float maximum = points.Count - 1;
                fromPosition = Mathf.Clamp(fromPosition, 0.0f, maximum);
                entryPosition = Mathf.Clamp(entryPosition, 0.0f, maximum);
                float delta = increasing ? entryPosition - fromPosition
                                         : fromPosition - entryPosition;
                if (delta < -0.0001f && IsClosed(points)) delta += maximum;
                if (delta < -0.0001f) return false;
                float position = fromPosition;
                while (delta > 0.0001f)
                {
                    int index;
                    float fraction;
                    if (!increasing && Mathf.Abs(position - Mathf.Round(position)) < 0.0001f &&
                        position > 0.0f)
                    {
                        index = Mathf.FloorToInt(position) - 1;
                        fraction = 1.0f;
                    }
                    else
                    {
                        index = Mathf.Min(points.Count - 2, Mathf.FloorToInt(position));
                        fraction = position - Mathf.Floor(position);
                    }
                    float step = Mathf.Min(delta, increasing ? 1.0f - fraction : fraction);
                    float dx = points[index + 1].x - points[index].x;
                    float dz = points[index + 1].z - points[index].z;
                    distance += step * Mathf.Sqrt(dx * dx + dz * dz);
                    delta -= step;
                    position += increasing ? step : -step;
                    if (position >= maximum - 0.0001f && delta > 0.0001f) position = 0.0f;
                    if (position <= 0.0001f && delta > 0.0001f && !increasing) position = maximum;
                }
                return true;
            }
        }

        private static List<LineRoutePointDto> CaptureCurvePoints(TransportManager manager,
                                                                  ushort lineId,
                                                                  VehicleInfo.VehicleType vehicleType)
        {
            List<LineRoutePointDto> points = new List<LineRoutePointDto>();
            if (manager.m_lineCurves == null ||
                lineId >= manager.m_lineCurves.Length ||
                manager.m_lineCurves[lineId] == null)
            {
                return points;
            }

            Bezier3[] curves = manager.m_lineCurves[lineId];
            for (int curveIndex = 0; curveIndex < curves.Length; ++curveIndex)
            {
                Bezier3 curve = curves[curveIndex];
                int sampleCount = AdaptiveSampleCount(curve);
                int firstSample = curveIndex == 0 ? 0 : 1;
                for (int sample = firstSample; sample <= sampleCount; ++sample)
                {
                    float t = (float)sample / (float)sampleCount;
                    Vector3 position = curve.Position(t);
                    AddPoint(points, "c" + curveIndex + "_" + sample,
                             position, vehicleType);
                }
            }

            return points;
        }

        private static int AdaptiveSampleCount(Bezier3 curve)
        {
            Vector3 previous = curve.Position(0.0f);
            float polylineLength = 0.0f;
            for (int i = 1; i <= 8; ++i)
            {
                Vector3 current = curve.Position(i / 8.0f);
                polylineLength += Vector3.Distance(previous, current);
                previous = current;
            }
            int samples = Mathf.CeilToInt(polylineLength / 15.0f);
            return Mathf.Clamp(samples, 4, 32);
        }

        private static bool IsClosed(List<LineRoutePointDto> points)
        {
            if (points == null || points.Count < 3) return false;
            LineRoutePointDto first = points[0];
            LineRoutePointDto last = points[points.Count - 1];
            float dx = first.x - last.x, dy = first.y - last.y, dz = first.z - last.z;
            return dx * dx + dy * dy + dz * dz <= 4.0f;
        }

        private static List<LineRoutePointDto> CaptureStopPoints(ushort lineId,
                                                                 TransportLine line,
                                                                 VehicleInfo.VehicleType vehicleType)
        {
            List<LineRoutePointDto> points = new List<LineRoutePointDto>();
            int count = line.CountStops(lineId);
            for (int index = 0; index < count; ++index)
            {
                ushort nodeId = line.GetStop(index);
                if (nodeId == 0) continue;
                NetNode node = NetManager.instance.m_nodes.m_buffer[nodeId];
                AddPoint(points, "stop_" + nodeId, node.m_position, vehicleType);
            }
            return points;
        }

        private static void AddPoint(List<LineRoutePointDto> points,
                                     string id,
                                     Vector3 position,
                                     VehicleInfo.VehicleType vehicleType)
        {
            if (points.Count > 0)
            {
                LineRoutePointDto previous = points[points.Count - 1];
                float dx = previous.x - position.x;
                float dy = previous.y - position.y;
                float dz = previous.z - position.z;
                if (dx * dx + dy * dy + dz * dz <
                    DuplicatePointTolerance * DuplicatePointTolerance)
                {
                    return;
                }
            }

            LineRoutePointDto point = new LineRoutePointDto();
            point.id = id;
            point.track_segment_id = ClosestTrackSegmentId(position, vehicleType);
            point.x = position.x;
            point.y = position.y;
            point.z = position.z;
            points.Add(point);
        }

        private static string ClosestTrackSegmentId(
            Vector3 position, VehicleInfo.VehicleType vehicleType)
        {
            ushort[] candidates = new ushort[16];
            int count = 0;
            NetManager manager = NetManager.instance;
            manager.GetClosestSegments(position, candidates, out count);
            ushort best = 0;
            float bestDistanceSq = 16.0f;
            for (int i = 0; i < count && i < candidates.Length; ++i)
            {
                ushort segmentId = candidates[i];
                if (segmentId == 0) continue;
                NetSegment segment = manager.m_segments.m_buffer[segmentId];
                if ((segment.m_flags & NetSegment.Flags.Created) == 0) continue;
                NetInfo netInfo = segment.Info;
                if (netInfo == null ||
                    (netInfo.m_vehicleTypes & vehicleType) == 0) continue;
                Vector3 closest = segment.GetClosestPosition(position);
                float distanceSq = (closest - position).sqrMagnitude;
                if (distanceSq < bestDistanceSq)
                {
                    bestDistanceSq = distanceSq;
                    best = segmentId;
                }
            }
            return best == 0 ? String.Empty : "net:" + best.ToString();
        }

        private static string NormalizeTransportType(TransportInfo info)
        {
            if (info == null) return "unknown";
            return info.m_transportType.ToString().ToLowerInvariant();
        }

        private static float DistanceToSegmentSq(float px, float pz,
                                                 float ax, float az,
                                                 float bx, float bz)
        {
            float abx = bx - ax;
            float abz = bz - az;
            float apx = px - ax;
            float apz = pz - az;
            float denom = abx * abx + abz * abz;
            float t = denom <= 0.0001f ? 0.0f : (apx * abx + apz * abz) / denom;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            float cx = ax + t * abx;
            float cz = az + t * abz;
            float dx = px - cx;
            float dz = pz - cz;
            return dx * dx + dz * dz;
        }

        private static long UnixTimeMilliseconds()
        {
            DateTime epoch = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc);
            return (long)(DateTime.UtcNow - epoch).TotalMilliseconds;
        }
    }
}
