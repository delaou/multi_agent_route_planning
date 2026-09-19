using System;
using System.Collections.Generic;
using UnityEngine;

namespace VehicleTelemetryMod
{
    internal static class VehicleSnapshotCollector
    {
        private const float WorldHalfSize = 8640f;

        public static VehicleSnapshot Capture()
        {
            List<VehicleDto> vehicles = new List<VehicleDto>();
            Vehicle[] buffer = VehicleManager.instance.m_vehicles.m_buffer;

            for (int index = 1; index < buffer.Length; ++index)
            {
                ushort vehicleId = (ushort)index;
                Vehicle vehicle = buffer[vehicleId];
                if ((vehicle.m_flags & Vehicle.Flags.Created) == 0)
                {
                    continue;
                }

                VehicleInfo info = vehicle.Info;
                if (info == null || info.m_class == null)
                {
                    continue;
                }

                string service = NormalizeService(info.m_class.m_service.ToString());
                if (service == null)
                {
                    continue;
                }

                Vector3 position = vehicle.GetLastFramePosition();
                Vector3 velocity = vehicle.GetLastFrameVelocity();
                Vehicle.Frame frame = vehicle.GetLastFrameData();
                Quaternion rotation = frame.m_rotation;

                VehicleDto dto = new VehicleDto();
                dto.id = vehicleId.ToString();
                dto.service = service;
                dto.prefab = info.name;
                dto.status = vehicle.m_flags.ToString();
                dto.position = new PositionDto();
                dto.position.x = position.x;
                dto.position.y = position.y;
                dto.position.z = position.z;
                dto.speed = velocity.magnitude;
                dto.heading = rotation.eulerAngles.y;
                vehicles.Add(dto);
            }

            VehicleSnapshot snapshot = new VehicleSnapshot();
            snapshot.timestamp = UnixTimeMilliseconds();
            snapshot.map = new MapBoundsDto();
            snapshot.map.min_x = -WorldHalfSize;
            snapshot.map.max_x = WorldHalfSize;
            snapshot.map.min_z = -WorldHalfSize;
            snapshot.map.max_z = WorldHalfSize;
            snapshot.vehicles = vehicles;
            return snapshot;
        }

        private static string NormalizeService(string service)
        {
            switch (service)
            {
                case "FireDepartment": return "fire";
                case "PoliceDepartment": return "police";
                case "Healthcare": return "healthcare";
                case "Garbage": return "garbage";
                case "Road": return "road";
                case "PublicTransport": return "public_transport";
                case "Disaster": return "disaster";
                case "Water": return "water";
                case "Electricity": return "electricity";
                case "Beautification": return "park";
                default: return null;
            }
        }

        private static long UnixTimeMilliseconds()
        {
            DateTime epoch = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc);
            return (long)(DateTime.UtcNow - epoch).TotalMilliseconds;
        }
    }
}
