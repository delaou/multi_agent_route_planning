using System;
using System.Threading;
using UnityEngine;

namespace VehicleTelemetryMod
{
    internal static class DispatchCommandPoller
    {
        private static readonly object SyncRoot = new object();
        private static Thread worker;
        private static bool running;
        private static string lastResponse = string.Empty;

        public static void Start()
        {
            lock (SyncRoot)
            {
                if (running)
                {
                    return;
                }

                running = true;
                lastResponse = string.Empty;
                worker = new Thread(WorkerLoop);
                worker.IsBackground = true;
                worker.Name = "DispatchCommandPoller";
                worker.Start();
                ModLog.Write("DispatchCommandPoller started");
            }
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
                threadToJoin = worker;
                worker = null;
            }

            if (threadToJoin != null && threadToJoin.IsAlive)
            {
                threadToJoin.Join(2500);
            }
        }

        private static void WorkerLoop()
        {
            int pollCount = 0;
            while (IsRunning())
            {
                try
                {
                    // 首轮先拿到拓扑再接收 RELEASE，防止游戏端在不知道入口位置时把
                    // 列车误当作已获准通行。之后每 4 轮（约 2 秒）刷新一次。
                    if ((pollCount % 4) == 0)
                    {
                        DispatchRuntime.AcceptTopologyResponse(
                            TelemetrySender.QueryTopology());
                    }

                    string response = TelemetrySender.QueryDispatchCommands();
                    DispatchRuntime.AcceptCommandResponse(response);
                    if (response != lastResponse)
                    {
                        lastResponse = response;
                        ModLog.Write("Dispatch commands response="
                                     + TrimForLog(response));
                    }

                    ++pollCount;
                }
                catch (Exception exception)
                {
                    ModLog.Write("Dispatch command query failed: " + exception);
                    Debug.LogWarning("[VehicleTelemetry] Dispatch query failed: "
                                     + exception.Message);
                }

                Thread.Sleep(500);
            }
        }

        private static bool IsRunning()
        {
            lock (SyncRoot)
            {
                return running;
            }
        }

        private static string TrimForLog(string text)
        {
            if (text == null)
            {
                return string.Empty;
            }
            if (text.Length <= 512)
            {
                return text;
            }
            return text.Substring(0, 512) + "...";
        }
    }
}
