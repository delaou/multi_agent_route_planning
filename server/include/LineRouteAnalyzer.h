#pragma once

#include "IntersectionReservationManager.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// 游戏 Mod 沿线路实际路径采样得到的三维有序点。
struct LineRoutePoint {
    std::string point_id;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    // Cities: Skylines 实体 NetSegment；为空时才回退到严格几何匹配。
    std::string track_segment_id;
};

// 一条运营线路的完整路径。points 顺序必须与车辆 route_index 一致。
struct LineRoute {
    std::string line_id;
    std::string line_name;
    std::string transport_type;
    std::string geometry_source = "unknown";
    bool is_loop = false;
    std::vector<LineRoutePoint> points;
    uint64_t target_headway_ms = 15000;
};

// 某线路的点序方向与共线段规范方向相同或相反。
enum class RouteDirectionRelation { SAME, OPPOSITE };

// 某条线路在共线段中的索引范围及相对于共线段标准方向的方向关系。
struct SharedCorridorMember {
    std::string line_id;
    // 沿线路点序的连续位置，整数部分是 segment， 小数部分是 segment 内 t。
    double start_position = 0.0;
    double end_position = 0.0;
    // 同一线路多次经过相同几何时用于区分去程、返程或不同圈段。
    std::string occurrence_id;
    size_t start_segment = 0;
    size_t end_segment = 0;
    RouteDirectionRelation direction_relation = RouteDirectionRelation::SAME;
};

// 多条线路连续重合形成的共线走廊；三条以上线路会合并到同一定义。
struct SharedCorridorDefinition {
    std::string corridor_id;
    double start_x = 0.0;
    double start_y = 0.0;
    double start_z = 0.0;
    double end_x = 0.0;
    double end_y = 0.0;
    double end_z = 0.0;
    double length = 0.0;
    // 分析器直接输出的实际匹配折线，前端与调度共享同一几何边界。
    std::vector<LineRoutePoint> geometry;
    std::vector<SharedCorridorMember> members;
};

// 线路折线在三维高度容差内形成的实际交叉点。
struct LineIntersectionDefinition {
    std::string intersection_id;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    std::string line_a_id;
    std::string line_b_id;
    size_t line_a_segment = 0;
    size_t line_b_segment = 0;
    std::string line_a_movement_id;
    std::string line_b_movement_id;
};

// 一次完整几何分析生成的共线段、交叉点及互斥 movement。
struct LineTopologyAnalysis {
    std::vector<SharedCorridorDefinition> shared_corridors;
    std::vector<LineIntersectionDefinition> intersections;
    std::vector<LineIntersectionMovement> movements;
};

// 控制几何去噪、高架分离和短重合过滤的分析参数。
struct LineRouteAnalyzerConfig {
    // x/z 平面的路径点合并容差。
    double point_merge_tolerance = 5.0;
    // 共轨候选使用更严格的中心线距离；避免把相邻上下行轨道当成同轨。
    double shared_track_tolerance = 1.25;
    // 高度差超过该值时视为高架或隧道跨越，不产生冲突/共线。
    double vertical_separation_tolerance = 3.0;
    // 排除非常短的局部重合，避免将普通交点误判为共线段。
    double shared_corridor_min_length = 60.0;
    // 小于该锐角的线段按近似平行处理，不生成普通交叉点；近似平行且
    // 投影重叠、线间距和高度满足容差时进入共线段识别。
    double intersection_min_angle_degrees = 15.0;
};

inline constexpr uint64_t LINE_TOPOLOGY_ALGORITHM_VERSION = 3;

// 对多条有序三维折线执行共线与交叉关系提取的无状态分析器。
class LineRouteAnalyzer {
public:
    // 保存并校验水平容差、高度容差和最短共线长度。
    explicit LineRouteAnalyzer(LineRouteAnalyzerConfig config = {});
    // 对线路两两进行三维折线分析，输出连续共线段、孤立交叉点以及
    // 互斥 movement；不保存运行时列车或预约状态。
    LineTopologyAnalysis analyze(const std::vector<LineRoute>& routes) const;

private:
    LineRouteAnalyzerConfig config_;
};
