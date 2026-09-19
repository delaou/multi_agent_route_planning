#include "LineTopologyRegistry.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <utility>

// 构造时只配置分析器；真正的几何计算在 replaceRoutes 中按快照触发。
LineTopologyRegistry::LineTopologyRegistry(LineRouteAnalyzerConfig config)
    : analyzer_(config) {
    std::ostringstream value;
    value << "v" << LINE_TOPOLOGY_ALGORITHM_VERSION << std::fixed
          << std::setprecision(3) << ':' << config.point_merge_tolerance << ':'
          << config.shared_track_tolerance << ':'
          << config.vertical_separation_tolerance << ':'
          << config.shared_corridor_min_length << ':'
          << config.intersection_min_angle_degrees;
    algorithm_fingerprint_ = value.str();
}

// 先在局部变量中完成校验、几何分析和反向索引构建，最后短时间加锁交换。
// 任何一步失败都直接返回，使读线程继续使用上一版完整拓扑。
bool LineTopologyRegistry::replaceRoutes(const std::vector<LineRoute>& routes,
                                         std::string* reason) {
    std::unordered_map<std::string, LineRoute> new_routes;
    for (const LineRoute& route : routes) {
        if (route.line_id.empty() || route.points.size() < 2 ||
            route.target_headway_ms == 0) {
            if (reason) *reason = "invalid_line_route";
            return false;
        }
        if (!new_routes.emplace(route.line_id, route).second) {
            if (reason) *reason = "duplicate_line_id:" + route.line_id;
            return false;
        }
        for (const LineRoutePoint& point : route.points) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                !std::isfinite(point.z)) {
                if (reason) *reason = "invalid_line_point:" + route.line_id;
                return false;
            }
        }
    }

    // 几何分析可能较耗时，先在锁外完成，避免阻塞列车遥测读取。
    LineTopologyAnalysis new_analysis = analyzer_.analyze(routes);
    std::unordered_map<std::string, std::vector<CorridorTraversal>>
        new_corridors_by_line;
    std::unordered_map<std::string, std::vector<IntersectionTraversal>>
        new_intersections_by_line;

    for (const SharedCorridorDefinition& corridor :
         new_analysis.shared_corridors) {
        for (const SharedCorridorMember& member : corridor.members) {
            new_corridors_by_line[member.line_id].push_back(CorridorTraversal{
                corridor.corridor_id, member.line_id, member.start_segment,
                member.end_segment,
                std::min(member.start_position, member.end_position),
                std::max(member.start_position, member.end_position),
                member.occurrence_id, member.direction_relation});
        }
    }
    for (const LineIntersectionDefinition& intersection :
         new_analysis.intersections) {
        new_intersections_by_line[intersection.line_a_id].push_back(
            IntersectionTraversal{intersection.intersection_id,
                intersection.line_a_movement_id, intersection.line_a_id,
                intersection.line_a_segment});
        new_intersections_by_line[intersection.line_b_id].push_back(
            IntersectionTraversal{intersection.intersection_id,
                intersection.line_b_movement_id, intersection.line_b_id,
                intersection.line_b_segment});
    }

    std::lock_guard<std::mutex> lock(mutex_);
    routes_.swap(new_routes);
    analysis_ = std::move(new_analysis);
    corridors_by_line_.swap(new_corridors_by_line);
    intersections_by_line_.swap(new_intersections_by_line);
    ++revision_;
    return true;
}

// 复用普通替换的完整校验和分析过程，再把自增版本校正为持久化版本。
bool LineTopologyRegistry::restoreRoutes(const std::vector<LineRoute>& routes,
                                         uint64_t revision,
                                         std::string* reason) {
    if (revision == 0) {
        if (reason) *reason = "invalid_topology_revision";
        return false;
    }
    if (!replaceRoutes(routes, reason)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    revision_ = revision;
    return true;
}

// O(1) 按 line_id 查询线路；返回副本避免暴露受 mutex_ 保护的内部容器。
std::optional<LineRoute> LineTopologyRegistry::route(
    const std::string& line_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = routes_.find(line_id);
    if (it == routes_.end()) return std::nullopt;
    return it->second;
}

// 利用反向索引查询线路涉及的全部共线段；没有记录时返回空数组而非报错。
std::vector<CorridorTraversal> LineTopologyRegistry::corridorsForLine(
    const std::string& line_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = corridors_by_line_.find(line_id);
    return it == corridors_by_line_.end()
        ? std::vector<CorridorTraversal>{} : it->second;
}

// 利用反向索引查询线路涉及的全部交叉 movement，供候选生成器做前向筛选。
std::vector<IntersectionTraversal> LineTopologyRegistry::intersectionsForLine(
    const std::string& line_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = intersections_by_line_.find(line_id);
    return it == intersections_by_line_.end()
        ? std::vector<IntersectionTraversal>{} : it->second;
}

// 一次性复制线路和分析结果，保证调用者拿到的是同一拓扑版本的自洽快照。
LineTopologySnapshot LineTopologyRegistry::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    LineTopologySnapshot result;
    result.revision = revision_;
    result.algorithm_fingerprint = algorithm_fingerprint_;
    result.analysis = analysis_;
    result.routes.reserve(routes_.size());
    for (const auto& pair : routes_) result.routes.push_back(pair.second);
    std::sort(result.routes.begin(), result.routes.end(),
              [](const LineRoute& lhs, const LineRoute& rhs) {
                  return lhs.line_id < rhs.line_id;
              });
    return result;
}

LineTopologySummary LineTopologyRegistry::summary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    LineTopologySummary result;
    result.revision = revision_;
    result.algorithm_fingerprint = algorithm_fingerprint_;
    result.route_count = routes_.size();
    result.corridor_count = analysis_.shared_corridors.size();
    result.intersection_count = analysis_.intersections.size();
    result.movement_count = analysis_.movements.size();
    return result;
}
