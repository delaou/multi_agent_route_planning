using ICities;

namespace VehicleTelemetryMod
{
    public sealed class LoadingExtension : LoadingExtensionBase
    {
        public override void OnLevelLoaded(LoadMode mode)
        {
            base.OnLevelLoaded(mode);
            ModLog.Write("LoadingExtension.OnLevelLoaded mode=" + mode);
            DispatchRuntime.Reset();
            TelemetrySender.Start();
            DispatchCommandPoller.Start();
        }

        public override void OnLevelUnloading()
        {
            ModLog.Write("LoadingExtension.OnLevelUnloading");
            DispatchCommandPoller.Stop();
            DispatchRuntime.Reset();
            TelemetrySender.Stop();
            base.OnLevelUnloading();
        }
    }

    public sealed class TelemetryThreadingExtension : ThreadingExtensionBase
    {
        private const float VehicleUploadIntervalSeconds = 0.5f;
        private const float TrainUploadIntervalSeconds = 0.5f;
        private const float LineUploadIntervalSeconds = 10.0f;
        private float vehicleElapsed;
        private float trainElapsed;
        private float lineElapsed;
        private bool firstUpdate = true;
        private bool firstLineUpload = true;

        public override void OnUpdate(float realTimeDelta, float simulationTimeDelta)
        {
            // 所有 VehicleManager 读写都从游戏更新线程执行，网络线程只负责排队消息。
            DispatchRuntime.Tick();
            if (firstUpdate)
            {
                firstUpdate = false;
                ModLog.Write("TelemetryThreadingExtension.OnUpdate started");
            }

            vehicleElapsed += realTimeDelta;
            trainElapsed += realTimeDelta;
            lineElapsed += realTimeDelta;

            if (firstLineUpload || lineElapsed >= LineUploadIntervalSeconds)
            {
                firstLineUpload = false;
                lineElapsed = 0f;
                LineRouteSnapshot routes = LineRouteCollector.Capture();
                TelemetrySender.Enqueue(routes);
            }

            if (vehicleElapsed >= VehicleUploadIntervalSeconds)
            {
                vehicleElapsed = 0f;
                VehicleSnapshot snapshot = VehicleSnapshotCollector.Capture();
                TelemetrySender.Enqueue(snapshot);
            }

            if (trainElapsed >= TrainUploadIntervalSeconds)
            {
                trainElapsed = 0f;
                TrainSnapshot snapshot = TrainSnapshotCollector.Capture();
                DispatchRuntime.UpdateTrainSnapshot(snapshot);
                TelemetrySender.Enqueue(snapshot);
            }
        }
    }
}
