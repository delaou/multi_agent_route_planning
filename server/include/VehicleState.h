#pragma once

#include <cstdint>
#include <string>
#include <vector>

/** @brief 游戏地图坐标系中的三维车辆位置。 */
struct VehiclePosition {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

/** @brief 用于展示和查询的单辆道路车辆最新状态。 */
struct VehicleState {
    std::string id;
    std::string service;
    std::string prefab;
    std::string status;
    VehiclePosition position;
    double speed = 0.0;
    double heading = 0.0;
};

/** @brief 将游戏世界坐标映射到前端地图所需的 X/Z 边界。 */
struct GameMapBounds {
    double min_x = -8640.0;
    double max_x = 8640.0;
    double min_z = -8640.0;
    double max_z = 8640.0;
};

/** @brief 某一时刻游戏端上传的完整道路车辆集合。 */
struct VehicleSnapshot {
    uint64_t timestamp = 0;
    GameMapBounds map;
    std::vector<VehicleState> vehicles;
};
