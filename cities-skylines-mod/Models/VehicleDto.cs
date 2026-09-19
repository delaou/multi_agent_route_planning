using System.Collections.Generic;

namespace VehicleTelemetryMod
{
    [System.Serializable]
    public sealed class PositionDto
    {
        public float x;
        public float y;
        public float z;
    }

    [System.Serializable]
    public sealed class VehicleDto
    {
        public string id;
        public string service;
        public string prefab;
        public string status;
        public PositionDto position;
        public float speed;
        public float heading;
    }

    [System.Serializable]
    public sealed class MapBoundsDto
    {
        public float min_x;
        public float max_x;
        public float min_z;
        public float max_z;
    }

    [System.Serializable]
    public sealed class VehicleSnapshot
    {
        public long timestamp;
        public MapBoundsDto map;
        public List<VehicleDto> vehicles;
    }

    [System.Serializable]
    public sealed class LineRoutePointDto
    {
        public string id;
        public string track_segment_id;
        public float x;
        public float y;
        public float z;
    }

    [System.Serializable]
    public sealed class LineRouteDto
    {
        public string line_id;
        public string line_name;
        public string direction;
        public string transport_type;
        public string geometry_source;
        public bool is_loop;
        public long version;
        public long updated_at;
        public long target_headway_ms;
        public List<LineRoutePointDto> points;
    }

    [System.Serializable]
    public sealed class LineRouteSnapshot
    {
        public long timestamp;
        public long version;
        public List<LineRouteDto> routes;
    }

    [System.Serializable]
    public sealed class TrainPositionDto
    {
        public float x;
        public float y;
        public float z;
    }

    [System.Serializable]
    public sealed class TrainDto
    {
        public string train_id;
        public string line_id;
        public string travel_direction;
        public string status;
        public TrainPositionDto position;
        public float speed;
        public int route_index;
        public float route_position;
    }

    [System.Serializable]
    public sealed class TrainSnapshot
    {
        public long simulation_time_ms;
        public List<TrainDto> trains;
    }

    [System.Serializable]
    public sealed class DispatchCommandDto
    {
        public string command_id;
        public string action;
        public string corridor_resource_id;
        public string corridor_id;
        public string direction;
        public string train_id;
        public string line_id;
        public string reason;
        public long valid_until_simulation_ms;
        public long topology_revision;
        public long issued_at_simulation_ms;
        public bool has_route_bounds;
        public int entry_route_index;
        public int exit_route_index;
        public float entry_route_position;
        public float exit_route_position;
    }

    [System.Serializable]
    public sealed class DispatchCommandResponseDto
    {
        public bool ok;
        public int command_count;
        public List<DispatchCommandDto> commands;
    }

    [System.Serializable]
    public sealed class TopologyCorridorMemberDto
    {
        public string line_id;
        public int start_segment;
        public int end_segment;
        public float start_position;
        public float end_position;
        public string occurrence_id;
        public string direction_relation;
    }

    [System.Serializable]
    public sealed class TopologyCorridorDto
    {
        public string corridor_id;
        public List<TopologyCorridorMemberDto> members;
    }

    [System.Serializable]
    public sealed class TopologyResponseDto
    {
        public bool ok;
        public long topology_revision;
        public long topology_algorithm_version;
        public List<TopologyCorridorDto> corridors;
    }
}
