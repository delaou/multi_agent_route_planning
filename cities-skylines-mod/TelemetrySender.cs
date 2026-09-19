using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Net;
using System.Text;
using System.Threading;
using UnityEngine;

namespace VehicleTelemetryMod
{
    internal static class TelemetrySender
    {
        private static readonly string ServerBaseUrl = ResolveServerBaseUrl();
        private const string VehicleSnapshotPath = "/api/game/vehicles/snapshot";
        private const string TrainSnapshotPath = "/api/game/trains/snapshot";
        private const string LineSnapshotPath = "/api/game/lines/snapshot";
        private const string DispatchCommandQueryPath = "/api/game/dispatch/commands/query";
        private const string DispatchCommandAckPath = "/api/game/dispatch/commands/ack";
        private const string DispatchResourceClearedPath =
            "/api/game/dispatch/resources/cleared";
        private const string TopologyQueryPath = "/api/game/topology";

        private static readonly object SyncRoot = new object();
        private static readonly AutoResetEvent PayloadReady = new AutoResetEvent(false);
        private static readonly Dictionary<string, string> LatestPayloadByPath =
            new Dictionary<string, string>();
        // ACK/cleared 是不可合并的状态迁移事件，必须按条保留；普通遥测则只保留最新帧。
        private static readonly LinkedList<ControlPost> PendingControlPosts =
            new LinkedList<ControlPost>();
        private static Thread worker;
        private static bool running;

        private sealed class ControlPost
        {
            public string Path;
            public string Json;
            public int Attempts;
        }

        private static string ResolveServerBaseUrl()
        {
            string configured = Environment.GetEnvironmentVariable(
                "CS_TELEMETRY_SERVER_URL");
            if (String.IsNullOrEmpty(configured))
            {
                return "http://127.0.0.1:8080";
            }

            return configured.Trim().TrimEnd('/');
        }

        public static void Start()
        {
            lock (SyncRoot)
            {
                if (running)
                {
                    return;
                }

                running = true;
                LatestPayloadByPath.Clear();
                PendingControlPosts.Clear();
                worker = new Thread(WorkerLoop);
                worker.IsBackground = true;
                worker.Name = "CitiesTelemetrySender";
                worker.Start();
                ModLog.Write("TelemetrySender started baseUrl=" + ServerBaseUrl);
            }
        }

        public static void Enqueue(VehicleSnapshot snapshot)
        {
            if (snapshot == null) return;
            EnqueueJson(VehicleSnapshotPath, Serialize(snapshot));
        }

        public static void Enqueue(LineRouteSnapshot snapshot)
        {
            if (snapshot == null) return;
            EnqueueJson(LineSnapshotPath, Serialize(snapshot));
        }

        public static void Enqueue(TrainSnapshot snapshot)
        {
            if (snapshot == null) return;
            EnqueueJson(TrainSnapshotPath, Serialize(snapshot));
        }

        public static string QueryDispatchCommands()
        {
            return PostForResponse(DispatchCommandQueryPath, "{}");
        }

        public static string QueryTopology()
        {
            return GetForResponse(TopologyQueryPath);
        }

        /// <summary>
        /// 排队发送进入 ACK。此方法从游戏线程调用，但实际 HTTP I/O 在发送线程完成，
        /// 因而服务器短暂卡顿不会冻结游戏仿真。
        /// </summary>
        public static void EnqueueDispatchAck(string commandId,
                                              string trainId,
                                              string corridorResourceId,
                                              bool accepted,
                                              long simulationTimeMs)
        {
            StringBuilder json = new StringBuilder(320);
            json.Append('{');
            AppendString(json, "command_id", commandId, false);
            AppendString(json, "train_id", trainId, true);
            AppendString(json, "corridor_resource_id", corridorResourceId, true);
            json.Append(",\"accepted\":").Append(accepted ? "true" : "false");
            json.Append(",\"simulation_time_ms\":").Append(simulationTimeMs);
            json.Append('}');
            EnqueueControlPost(DispatchCommandAckPath, json.ToString());
        }

        /// <summary>车尾离开后通知服务器释放共线段及关联交叉点。</summary>
        public static void EnqueueResourceCleared(string trainId,
                                                  string corridorResourceId,
                                                  long simulationTimeMs)
        {
            StringBuilder json = new StringBuilder(256);
            json.Append('{');
            AppendString(json, "train_id", trainId, false);
            AppendString(json, "corridor_resource_id", corridorResourceId, true);
            json.Append(",\"simulation_time_ms\":").Append(simulationTimeMs);
            json.Append('}');
            EnqueueControlPost(DispatchResourceClearedPath, json.ToString());
        }

        public static void Stop()
        {
            Thread threadToJoin;
            lock (SyncRoot)
            {
                if (!running)
                {
                    return;
                }

                running = false;
                LatestPayloadByPath.Clear();
                PendingControlPosts.Clear();
                threadToJoin = worker;
                worker = null;
            }

            PayloadReady.Set();
            if (threadToJoin != null && threadToJoin.IsAlive)
            {
                threadToJoin.Join(2500);
            }
        }

        private static void EnqueueJson(string path, string json)
        {
            lock (SyncRoot)
            {
                if (!running)
                {
                    return;
                }

                // Keep only the newest payload per endpoint. Old telemetry frames
                // have no value once a newer snapshot exists.
                LatestPayloadByPath[path] = json;
            }

            PayloadReady.Set();
        }

        private static void EnqueueControlPost(string path, string json)
        {
            lock (SyncRoot)
            {
                if (!running) return;
                ControlPost post = new ControlPost();
                post.Path = path;
                post.Json = json;
                PendingControlPosts.AddLast(post);
            }
            PayloadReady.Set();
        }

        private static void WorkerLoop()
        {
            while (true)
            {
                PayloadReady.WaitOne();

                Dictionary<string, string> payloads;
                ControlPost controlPost = null;
                lock (SyncRoot)
                {
                    if (!running)
                    {
                        return;
                    }

                    payloads = new Dictionary<string, string>(LatestPayloadByPath);
                    LatestPayloadByPath.Clear();
                    if (PendingControlPosts.Count > 0)
                    {
                        // 每次只取队首一个控制事件。这样 ACK 尚未成功时，排在它后面的
                        // cleared 不会越过 ACK 到达服务器。
                        controlPost = PendingControlPosts.First.Value;
                        PendingControlPosts.RemoveFirst();
                    }
                }

                foreach (KeyValuePair<string, string> item in payloads)
                {
                    try
                    {
                        PostForResponse(item.Key, item.Value);
                        ModLog.Write("Uploaded " + item.Key);
                    }
                    catch (Exception exception)
                    {
                        ModLog.Write("Upload failed " + item.Key + ": " + exception);
                        Debug.LogWarning("[VehicleTelemetry] Upload failed "
                                         + item.Key + ": " + exception.Message);
                    }
                }

                if (controlPost != null)
                {
                    SendControlPost(controlPost);
                    // 成功时继续处理下一个；失败时 RetryControlPost 已把本条放回队首。
                    PayloadReady.Set();
                }
            }
        }

        private static void SendControlPost(ControlPost post)
        {
            try
            {
                PostForResponse(post.Path, post.Json);
                ModLog.Write("Control event uploaded " + post.Path);
            }
            catch (WebException exception)
            {
                // 4xx 通常表示服务器已处理过事件或其状态已改变，继续重试无意义。
                // 连接失败、超时等瞬态故障持续退避重试，避免 ACK/cleared 静默丢失。
                if (exception.Status == WebExceptionStatus.ProtocolError)
                {
                    if (exception.Response != null) exception.Response.Close();
                    ModLog.Write("Control event rejected " + post.Path + ": "
                                 + exception.Message);
                    return;
                }
                RetryControlPost(post, exception);
            }
            catch (Exception exception)
            {
                RetryControlPost(post, exception);
            }
        }

        private static void RetryControlPost(ControlPost post, Exception exception)
        {
            ++post.Attempts;
            lock (SyncRoot)
            {
                if (!running) return;
                // 放回队首维持每列车状态迁移的因果顺序：entry ACK 必须先于 cleared。
                PendingControlPosts.AddFirst(post);
            }
            ModLog.Write("Control event retry " + post.Attempts + " " + post.Path
                         + ": " + exception.Message);
            // Worker 上的小退避不会阻塞游戏线程；Set 使下轮循环重新取队列。
            Thread.Sleep(Math.Min(5000, 200 * post.Attempts));
            PayloadReady.Set();
        }

        private static string Serialize(VehicleSnapshot snapshot)
        {
            StringBuilder json = new StringBuilder(4096);
            json.Append("{\"timestamp\":").Append(snapshot.timestamp);
            json.Append(",\"map\":{");
            AppendNumber(json, "min_x", snapshot.map.min_x, false);
            AppendNumber(json, "max_x", snapshot.map.max_x, true);
            AppendNumber(json, "min_z", snapshot.map.min_z, true);
            AppendNumber(json, "max_z", snapshot.map.max_z, true);
            json.Append("},\"vehicles\":[");

            for (int i = 0; i < snapshot.vehicles.Count; ++i)
            {
                if (i > 0) json.Append(',');
                VehicleDto vehicle = snapshot.vehicles[i];
                json.Append('{');
                AppendString(json, "id", vehicle.id, false);
                AppendString(json, "service", vehicle.service, true);
                AppendString(json, "prefab", vehicle.prefab, true);
                AppendString(json, "status", vehicle.status, true);
                json.Append(",\"position\":{");
                AppendNumber(json, "x", vehicle.position.x, false);
                AppendNumber(json, "y", vehicle.position.y, true);
                AppendNumber(json, "z", vehicle.position.z, true);
                json.Append('}');
                AppendNumber(json, "speed", vehicle.speed, true);
                AppendNumber(json, "heading", vehicle.heading, true);
                json.Append('}');
            }

            json.Append("]}");
            return json.ToString();
        }

        private static string Serialize(LineRouteSnapshot snapshot)
        {
            StringBuilder json = new StringBuilder(8192);
            json.Append("{\"timestamp\":").Append(snapshot.timestamp);
            json.Append(",\"version\":").Append(snapshot.version);
            json.Append(",\"routes\":[");

            for (int i = 0; i < snapshot.routes.Count; ++i)
            {
                if (i > 0) json.Append(',');
                LineRouteDto route = snapshot.routes[i];
                json.Append('{');
                AppendString(json, "line_id", route.line_id, false);
                AppendString(json, "line_name", route.line_name, true);
                AppendString(json, "direction", route.direction, true);
                AppendString(json, "transport_type", route.transport_type, true);
                json.Append(",\"version\":").Append(route.version);
                json.Append(",\"updated_at\":").Append(route.updated_at);
                json.Append(",\"target_headway_ms\":").Append(route.target_headway_ms);
                json.Append(",\"points\":[");
                for (int pointIndex = 0; pointIndex < route.points.Count; ++pointIndex)
                {
                    if (pointIndex > 0) json.Append(',');
                    LineRoutePointDto point = route.points[pointIndex];
                    json.Append('{');
                    AppendString(json, "id", point.id, false);
                    AppendNumber(json, "x", point.x, true);
                    AppendNumber(json, "y", point.y, true);
                    AppendNumber(json, "z", point.z, true);
                    json.Append('}');
                }
                json.Append("]}");
            }

            json.Append("]}");
            return json.ToString();
        }

        private static string Serialize(TrainSnapshot snapshot)
        {
            StringBuilder json = new StringBuilder(8192);
            json.Append("{\"simulation_time_ms\":").Append(snapshot.simulation_time_ms);
            json.Append(",\"trains\":[");

            for (int i = 0; i < snapshot.trains.Count; ++i)
            {
                if (i > 0) json.Append(',');
                TrainDto train = snapshot.trains[i];
                json.Append('{');
                AppendString(json, "train_id", train.train_id, false);
                AppendString(json, "line_id", train.line_id, true);
                AppendString(json, "travel_direction", train.travel_direction, true);
                AppendString(json, "status", train.status, true);
                json.Append(",\"route_index\":").Append(train.route_index);
                json.Append(",\"position\":{");
                AppendNumber(json, "x", train.position.x, false);
                AppendNumber(json, "y", train.position.y, true);
                AppendNumber(json, "z", train.position.z, true);
                json.Append('}');
                AppendNumber(json, "speed", train.speed, true);
                json.Append('}');
            }

            json.Append("]}");
            return json.ToString();
        }

        private static string PostForResponse(string path, string json)
        {
            byte[] body = Encoding.UTF8.GetBytes(json);
            HttpWebRequest request =
                (HttpWebRequest)WebRequest.Create(ServerBaseUrl + path);
            request.Method = "POST";
            request.ContentType = "application/json; charset=utf-8";
            request.ContentLength = body.Length;
            request.Timeout = 2000;
            request.ReadWriteTimeout = 2000;
            request.KeepAlive = true;
            request.Proxy = null;

            using (Stream stream = request.GetRequestStream())
            {
                stream.Write(body, 0, body.Length);
            }

            using (HttpWebResponse response = (HttpWebResponse)request.GetResponse())
            {
                if ((int)response.StatusCode < 200 || (int)response.StatusCode >= 300)
                {
                    throw new WebException("Server returned HTTP "
                                           + (int)response.StatusCode);
                }

                using (StreamReader reader = new StreamReader(response.GetResponseStream()))
                {
                    return reader.ReadToEnd();
                }
            }
        }

        private static string GetForResponse(string path)
        {
            HttpWebRequest request =
                (HttpWebRequest)WebRequest.Create(ServerBaseUrl + path);
            request.Method = "GET";
            request.Timeout = 2000;
            request.ReadWriteTimeout = 2000;
            request.KeepAlive = true;
            request.Proxy = null;

            using (HttpWebResponse response = (HttpWebResponse)request.GetResponse())
            using (StreamReader reader = new StreamReader(response.GetResponseStream()))
            {
                return reader.ReadToEnd();
            }
        }

        private static void AppendString(StringBuilder json, string key,
                                         string value, bool comma)
        {
            if (comma) json.Append(',');
            json.Append('\"').Append(key).Append("\":\"");
            AppendEscaped(json, value ?? string.Empty);
            json.Append('\"');
        }

        private static void AppendNumber(StringBuilder json, string key,
                                         double value, bool comma)
        {
            if (comma) json.Append(',');
            json.Append('\"').Append(key).Append("\":");
            json.Append(value.ToString("R", CultureInfo.InvariantCulture));
        }

        private static void AppendEscaped(StringBuilder json, string value)
        {
            foreach (char character in value)
            {
                switch (character)
                {
                    case '\\': json.Append("\\\\"); break;
                    case '\"': json.Append("\\\""); break;
                    case '\n': json.Append("\\n"); break;
                    case '\r': json.Append("\\r"); break;
                    case '\t': json.Append("\\t"); break;
                    default:
                        if (character < 32)
                            json.Append("\\u").Append(((int)character).ToString("x4"));
                        else
                            json.Append(character);
                        break;
                }
            }
        }
    }
}
