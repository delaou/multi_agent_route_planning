#include "LineRouteAnalyzer.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

using json = nlohmann::json;

namespace {
bool compatible(const SharedCorridorDefinition& a,
                const SharedCorridorDefinition& b, size_t index_gap,
                double endpoint_gap) {
    if (a.members.size() != b.members.size()) return false;
    for (const auto& am : a.members) {
        auto found = std::find_if(b.members.begin(), b.members.end(),
            [&](const auto& bm) {
                return bm.line_id == am.line_id &&
                       bm.direction_relation == am.direction_relation;
            });
        if (found == b.members.end()) return false;
        const size_t al = std::min(am.start_segment, am.end_segment);
        const size_t ah = std::max(am.start_segment, am.end_segment);
        const size_t bl = std::min(found->start_segment, found->end_segment);
        const size_t bh = std::max(found->start_segment, found->end_segment);
        const size_t gap = ah < bl ? bl - ah - 1 : (bh < al ? al - bh - 1 : 0);
        if (gap > index_gap) return false;
    }
    const auto near = [&](double ax, double ay, double az,
                          double bx, double by, double bz) {
        return std::hypot(ax - bx, az - bz) <= endpoint_gap &&
               std::abs(ay - by) <= 3.0;
    };
    return near(a.end_x, a.end_y, a.end_z,
                b.start_x, b.start_y, b.start_z) ||
           near(a.start_x, a.start_y, a.start_z,
                b.end_x, b.end_y, b.end_z);
}

size_t componentCount(const std::vector<SharedCorridorDefinition>& corridors,
                      size_t index_gap, double endpoint_gap) {
    std::vector<size_t> parent(corridors.size());
    std::iota(parent.begin(), parent.end(), 0);
    const auto root = [&](size_t value, const auto& self) -> size_t {
        return parent[value] == value ? value : parent[value] = self(parent[value], self);
    };
    for (size_t i = 0; i < corridors.size(); ++i) {
        for (size_t j = i + 1; j < corridors.size(); ++j) {
            if (!compatible(corridors[i], corridors[j], index_gap, endpoint_gap)) continue;
            const size_t ri = root(i, root);
            const size_t rj = root(j, root);
            if (ri != rj) parent[rj] = ri;
        }
    }
    size_t count = 0;
    for (size_t i = 0; i < parent.size(); ++i)
        if (root(i, root) == i) ++count;
    return count;
}
} // namespace

int main(int argc, char** argv) {
    json input;
    std::cin >> input;
    std::vector<LineRoute> routes;
    for (const auto& item : input.at("routes")) {
        LineRoute route;
        route.line_id = item.at("line_id").get<std::string>();
        route.line_name = item.value("line_name", "");
        route.target_headway_ms = item.value("target_headway_ms", 15000ULL);
        for (const auto& value : item.at("points")) {
            LineRoutePoint point;
            point.point_id = value.value("id", value.value("point_id", ""));
            point.x = value.at("x").get<double>();
            point.y = value.value("y", 0.0);
            point.z = value.at("z").get<double>();
            route.points.push_back(std::move(point));
        }
        routes.push_back(std::move(route));
    }
    LineRouteAnalyzerConfig config;
    if (argc > 1) config.shared_corridor_min_length = std::stod(argv[1]);
    const LineTopologyAnalysis result = LineRouteAnalyzer{config}.analyze(routes);
    std::cout << "routes=" << routes.size()
              << " corridors=" << result.shared_corridors.size()
              << " intersections=" << result.intersections.size() << '\n';
    for (size_t index_gap : {2U, 5U, 10U, 20U}) {
        for (double endpoint_gap : {20.0, 50.0, 100.0, 200.0}) {
            std::cout << "merge endpoint_gap=" << endpoint_gap
                      << " index_gap=" << index_gap << " components="
                      << componentCount(result.shared_corridors, index_gap,
                                        endpoint_gap) << '\n';
        }
    }
}
