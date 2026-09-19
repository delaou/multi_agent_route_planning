using System;
using System.IO;

namespace VehicleTelemetryMod
{
    internal static class ModLog
    {
        private static readonly object SyncRoot = new object();
        private static readonly string LogPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            @"Colossal Order\Cities_Skylines\VehicleTelemetryMod.log"
        );

        public static void Write(string message)
        {
            try
            {
                lock (SyncRoot)
                {
                    File.AppendAllText(
                        LogPath,
                        DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff") +
                        " " + message + Environment.NewLine
                    );
                }
            }
            catch
            {
                // Logging must never interrupt the simulation thread.
            }
        }
    }
}
