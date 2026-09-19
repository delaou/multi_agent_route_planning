#include "LineRouteAnalyzer.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

LineRoute route(const std::string& id,
                std::initializer_list<std::initializer_list<double>> points) {
    LineRoute result;
    result.line_id = id;
    for (const auto& values : points) {
        const std::vector<double> value(values);
        result.points.push_back(LineRoutePoint{"", value[0], value[1], value[2]});
    }
    return result;
}

bool require(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

} // namespace

int main() {
    LineRouteAnalyzerConfig config;
    config.point_merge_tolerance = 2.0;
    config.vertical_separation_tolerance = 1.0;
    config.shared_corridor_min_length = 20.0;
    config.intersection_min_angle_degrees = 15.0;
    LineRouteAnalyzer analyzer(config);

    // 相同轨迹使用完全不同的采样点，仍应合并成一个共线段。
    auto non_aligned = analyzer.analyze({
        route("A", {{0, 0, 0}, {40, 0, 0}}),
        route("B", {{0, 0, 0}, {9, 0, 0}, {23, 0, 0}, {40, 0, 0}})
    });
    if (!require(non_aligned.shared_corridors.size() == 1,
                 "non-aligned sampling was not recognized as one corridor") ||
        !require(non_aligned.intersections.empty(),
                 "shared path vertices leaked into intersections") ||
        !require(std::abs(non_aligned.shared_corridors[0].length - 40.0) < 1e-6,
                 "shared path length was counted incorrectly")) return 1;

    // 坐标完全相同但实体轨道编号不同，不能仅凭几何误判为同轨。
    LineRoute physical_a = route("PA", {{0, 0, 0}, {40, 0, 0}});
    LineRoute physical_b = route("PB", {{0, 0, 0}, {40, 0, 0}});
    for (auto& point : physical_a.points) point.track_segment_id = "net:10";
    for (auto& point : physical_b.points) point.track_segment_id = "net:11";
    auto distinct_tracks = analyzer.analyze({physical_a, physical_b});
    if (!require(distinct_tracks.shared_corridors.empty(),
                 "different physical track ids were merged")) return 1;

    // 点序相反时仍是同一共线段，同时保留方向关系。
    auto reversed = analyzer.analyze({
        route("A", {{0, 0, 0}, {40, 0, 0}}),
        route("B", {{40, 0, 0}, {18, 0, 0}, {0, 0, 0}})
    });
    if (!require(reversed.shared_corridors.size() == 1,
                 "reverse non-aligned sampling was not recognized") ||
        !require(reversed.shared_corridors[0].members[1].direction_relation ==
                     RouteDirectionRelation::OPPOSITE,
                 "reverse corridor direction relation was lost")) return 1;

    // 真正大角度穿越的线路应成为交叉点，而不是共线段。
    auto crossing = analyzer.analyze({
        route("A", {{-20, 0, 0}, {20, 0, 0}}),
        route("B", {{0, 0, -20}, {0, 0, 20}})
    });
    if (!require(crossing.shared_corridors.empty(),
                 "perpendicular crossing was classified as a corridor") ||
        !require(crossing.intersections.size() == 1,
                 "perpendicular crossing was not recognized")) return 1;

    // 高度不同的投影交叉不应产生冲突。
    auto grade_separated = analyzer.analyze({
        route("A", {{-20, 0, 0}, {20, 0, 0}}),
        route("B", {{0, 5, -20}, {0, 5, 20}})
    });
    if (!require(grade_separated.shared_corridors.empty() &&
                 grade_separated.intersections.empty(),
                 "grade-separated routes produced a conflict")) return 1;

    // 并轨端点形成的浅角接触既不够长，也不应被误报为普通交叉点。
    auto shallow_touch = analyzer.analyze({
        route("A", {{0, 0, 0}, {20, 0, 0}}),
        route("B", {{20, 0, 0}, {40, 0, 1}})
    });
    if (!require(shallow_touch.shared_corridors.empty(),
                 "shallow endpoint touch became a corridor") ||
        !require(shallow_touch.intersections.empty(),
                 "shallow endpoint touch became an intersection")) return 1;

    // 同一路径中夹入另一次远端重复匹配时，候选排序可能先切成多个片段；最终归并
    // 应拼回连续的 80m 走廊，并在归并后过滤孤立的 40m 重复片段。
    LineRouteAnalyzerConfig merge_config = config;
    merge_config.shared_corridor_min_length = 60.0;
    LineRouteAnalyzer merge_analyzer(merge_config);
    auto consolidated = merge_analyzer.analyze({
        route("A", {{0, 0, 0}, {40, 0, 0}, {80, 0, 0}}),
        route("B", {{0, 0, 0}, {40, 0, 0}, {80, 0, 0},
                    {500, 0, 500}, {0, 0, 0}, {40, 0, 0}})
    });
    if (!require(consolidated.shared_corridors.size() == 2,
                 "member-change boundary was not atomized")) return 1;
    double consolidated_length = 0.0;
    for (const auto& corridor : consolidated.shared_corridors)
        consolidated_length += corridor.length;
    if (!require(std::abs(consolidated_length - 80.0) < 1e-6,
                 "atomized corridor coverage is incorrect")) return 1;

    // 同一线路往返同一几何区间，两个经过必须保留，不能按 line_id 去重丢掉返程。
    auto return_trip = analyzer.analyze({
        route("A", {{0,0,0}, {100,0,0}}),
        route("B", {{0,0,0}, {100,0,0}, {0,0,0}})
    });
    if (!require(return_trip.shared_corridors.size() == 1,
                 "same physical track was split into overlapping resources") ||
        !require(return_trip.shared_corridors[0].members.size() == 3,
                 "outbound and inbound occurrences were collapsed")) return 1;
    bool same_direction = false, opposite_direction = false;
    for (const auto& corridor : return_trip.shared_corridors) {
        for (const auto& member : corridor.members) {
            if (member.line_id != "B") continue;
            same_direction |= member.direction_relation == RouteDirectionRelation::SAME;
            opposite_direction |= member.direction_relation == RouteDirectionRelation::OPPOSITE;
        }
    }
    if (!require(same_direction && opposite_direction, "return direction was lost")) return 1;

    // 相同端点的闭环，点序相同也不能因为 start == end 而翻转方向。
    auto loop = analyzer.analyze({
        route("A", {{0,0,0}, {100,0,0}, {100,0,100}, {0,0,100}, {0,0,0}}),
        route("B", {{0,0,0}, {100,0,0}, {100,0,100}, {0,0,100}, {0,0,0}}),
        route("C", {{0,0,0}, {100,0,0}, {100,0,100}, {0,0,100}, {0,0,0}})
    });
    if (!require(loop.shared_corridors.size() == 1, "identical loops did not merge")) return 1;
    for (const auto& member : loop.shared_corridors.front().members)
        if (!require(member.direction_relation == RouteDirectionRelation::SAME,
                     "closed loop direction was flipped by coincident endpoints")) return 1;
    std::cout << "LINE_ROUTE_ANALYZER_OK\n";
    return 0;
}
