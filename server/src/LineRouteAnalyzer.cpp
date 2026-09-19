#include "LineRouteAnalyzer.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <iomanip>
#include <map>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

// 计算两个三维点在地图水平 x/z 平面的距离，高度 y 由独立容差判断。
double distance2d(const LineRoutePoint& lhs, const LineRoutePoint& rhs) {
    // 水平重合和高度重合使用不同容差，因此这里只计算 x/z 距离。
    return std::hypot(lhs.x - rhs.x, lhs.z - rhs.z);
}

// 返回线路指定小段的水平长度，用于累计共线长度。
LineRoutePoint interpolate(const LineRoutePoint& start,
                           const LineRoutePoint& end, double t);

double segmentLength(const LineRoute& route, size_t segment_index) {
    return distance2d(route.points[segment_index], route.points[segment_index + 1]);
}

LineRoutePoint routePointAt(const LineRoute& route, double position) {
    const double maximum = static_cast<double>(route.points.size() - 1);
    position = std::max(0.0, std::min(maximum, position));
    size_t segment = std::min(route.points.size() - 2,
                              static_cast<size_t>(std::floor(position)));
    return interpolate(route.points[segment], route.points[segment + 1],
                       position - static_cast<double>(segment));
}

std::vector<LineRoutePoint> routeGeometry(const LineRoute& route,
                                          double start, double end) {
    if (end < start) std::swap(start, end);
    std::vector<LineRoutePoint> points{routePointAt(route, start)};
    for (size_t i = static_cast<size_t>(std::floor(start)) + 1;
         i < route.points.size() && static_cast<double>(i) < end; ++i)
        points.push_back(route.points[i]);
    points.push_back(routePointAt(route, end));
    return points;
}

void setMemberBounds(SharedCorridorMember& member, const LineRoute& route,
                     double start, double end) {
    member.start_position = std::min(start, end);
    member.end_position = std::max(start, end);
    member.start_segment = std::min(route.points.size() - 2,
        static_cast<size_t>(std::floor(member.start_position)));
    member.end_segment = std::min(route.points.size() - 2,
        static_cast<size_t>(std::floor(std::max(member.start_position,
                                                member.end_position - 1e-9))));
    std::ostringstream id;
    id << member.line_id << '@' << member.start_position << '-'
       << member.end_position;
    member.occurrence_id = id.str();
}

LineRoutePoint interpolate(const LineRoutePoint& start,
                           const LineRoutePoint& end, double t) {
    LineRoutePoint result;
    result.x = start.x + (end.x - start.x) * t;
    result.y = start.y + (end.y - start.y) * t;
    result.z = start.z + (end.z - start.z) * t;
    return result;
}

double clamp01(double value) {
    return std::max(0.0, std::min(1.0, value));
}

// 用扩张后的水平包围盒快速排除相距很远的小段。全量线路两两比较时，
// 绝大多数 segment 不可能相交或重合，应在角度和投影计算之前淘汰。
bool segmentBoundsNear(const LineRoutePoint& a0, const LineRoutePoint& a1,
                       const LineRoutePoint& b0, const LineRoutePoint& b1,
                       double tolerance) {
    const double a_min_x = std::min(a0.x, a1.x);
    const double a_max_x = std::max(a0.x, a1.x);
    const double a_min_z = std::min(a0.z, a1.z);
    const double a_max_z = std::max(a0.z, a1.z);
    const double b_min_x = std::min(b0.x, b1.x);
    const double b_max_x = std::max(b0.x, b1.x);
    const double b_min_z = std::min(b0.z, b1.z);
    const double b_max_z = std::max(b0.z, b1.z);
    return a_min_x <= b_max_x + tolerance &&
           b_min_x <= a_max_x + tolerance &&
           a_min_z <= b_max_z + tolerance &&
           b_min_z <= a_max_z + tolerance;
}

// 返回两条小段方向向量之间的锐角。反向线段与同向线段都以 0 度表示平行。
double segmentAcuteAngleDegrees(const LineRoutePoint& a0,
                                const LineRoutePoint& a1,
                                const LineRoutePoint& b0,
                                const LineRoutePoint& b1) {
    const double ax = a1.x - a0.x;
    const double az = a1.z - a0.z;
    const double bx = b1.x - b0.x;
    const double bz = b1.z - b0.z;
    const double length_product = std::hypot(ax, az) * std::hypot(bx, bz);
    if (length_product <= 1e-9) return 90.0;
    const double cosine = clamp01(std::abs(ax * bx + az * bz) /
                                  length_product);
    constexpr double radians_to_degrees = 57.2957795130823208768;
    return std::acos(cosine) * radians_to_degrees;
}

struct SegmentOverlap {
    int orientation = 0;
    double a_start_t = 0.0;
    double a_end_t = 0.0;
    double b_start_t = 0.0;
    double b_end_t = 0.0;
    LineRoutePoint start;
    LineRoutePoint end;
};

// 判断两条采样小段所代表的轨迹是否近似共线并存在投影重叠。这里不要求
// 两边的采样端点对齐，因此一个长 segment 可以与另一线路的多个短 segment 匹配。
bool segmentOverlap(const LineRoutePoint& a0, const LineRoutePoint& a1,
                    const LineRoutePoint& b0, const LineRoutePoint& b1,
                    double horizontal_tolerance, double vertical_tolerance,
                    double parallel_angle_degrees, SegmentOverlap* overlap) {
    const bool a_has_track = !a0.track_segment_id.empty() || !a1.track_segment_id.empty();
    const bool b_has_track = !b0.track_segment_id.empty() || !b1.track_segment_id.empty();
    if (a_has_track && b_has_track) {
        const bool shared_track =
            (!a0.track_segment_id.empty() &&
             (a0.track_segment_id == b0.track_segment_id ||
              a0.track_segment_id == b1.track_segment_id)) ||
            (!a1.track_segment_id.empty() &&
             (a1.track_segment_id == b0.track_segment_id ||
              a1.track_segment_id == b1.track_segment_id));
        if (!shared_track) return false;
    }
    if (!segmentBoundsNear(a0, a1, b0, b1, horizontal_tolerance)) return false;
    const double ax = a1.x - a0.x;
    const double az = a1.z - a0.z;
    const double bx = b1.x - b0.x;
    const double bz = b1.z - b0.z;
    const double length_a = std::hypot(ax, az);
    const double length_b = std::hypot(bx, bz);
    if (length_a <= 1e-9 || length_b <= 1e-9 ||
        segmentAcuteAngleDegrees(a0, a1, b0, b1) > parallel_angle_degrees) {
        return false;
    }

    const double ux = ax / length_a;
    const double uz = az / length_a;
    const double b0_projection = (b0.x - a0.x) * ux + (b0.z - a0.z) * uz;
    const double b1_projection = (b1.x - a0.x) * ux + (b1.z - a0.z) * uz;
    const double overlap_start = std::max(0.0, std::min(b0_projection, b1_projection));
    const double overlap_end = std::min(length_a, std::max(b0_projection, b1_projection));
    if (overlap_end - overlap_start <= 1e-6) return false;

    const auto matchedPoint = [&](double distance_along_a,
                                  LineRoutePoint* point_a,
                                  LineRoutePoint* point_b) {
        const double a_t = clamp01(distance_along_a / length_a);
        *point_a = interpolate(a0, a1, a_t);
        const double b_t = clamp01(
            ((point_a->x - b0.x) * bx + (point_a->z - b0.z) * bz) /
            (length_b * length_b));
        *point_b = interpolate(b0, b1, b_t);
        return distance2d(*point_a, *point_b) <= horizontal_tolerance &&
               std::abs(point_a->y - point_b->y) <= vertical_tolerance;
    };

    LineRoutePoint a_start, a_end, b_start, b_end;
    if (!matchedPoint(overlap_start, &a_start, &b_start) ||
        !matchedPoint(overlap_end, &a_end, &b_end)) {
        return false;
    }

    overlap->orientation = ax * bx + az * bz >= 0.0 ? 1 : -1;
    overlap->a_start_t = overlap_start / length_a;
    overlap->a_end_t = overlap_end / length_a;
    overlap->b_start_t = clamp01(((a_start.x - b0.x) * bx +
                                  (a_start.z - b0.z) * bz) /
                                 (length_b * length_b));
    overlap->b_end_t = clamp01(((a_end.x - b0.x) * bx +
                                (a_end.z - b0.z) * bz) /
                               (length_b * length_b));
    overlap->start = interpolate(a_start, b_start, 0.5);
    overlap->end = interpolate(a_end, b_end, 0.5);
    return true;
}

// 为一对线路 segment 生成集合键，用于从孤立交点检测中排除已知共线段。
uint64_t segmentPairKey(size_t a, size_t b) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(a)) << 32) |
           static_cast<uint32_t>(b);
}

uint64_t gridKey(int64_t x, int64_t z) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
           static_cast<uint32_t>(z);
}

// 对 B 线路建立临时均匀网格，只把空间上相邻的 segment 交给后续精确计算。
// 返回值按 A 的 segment 索引组织，并已去重和排序，供共线、交叉两遍复用。
std::vector<std::vector<size_t>> spatialCandidates(
    const LineRoute& a, const LineRoute& b, double tolerance) {
    const double cell_size = std::max(32.0, tolerance * 4.0);
    const auto cell = [cell_size](double value) {
        return static_cast<int64_t>(std::floor(value / cell_size));
    };
    std::unordered_map<uint64_t, std::vector<size_t>> grid;
    for (size_t bj = 0; bj + 1 < b.points.size(); ++bj) {
        const int64_t min_x = cell(std::min(b.points[bj].x, b.points[bj + 1].x));
        const int64_t max_x = cell(std::max(b.points[bj].x, b.points[bj + 1].x));
        const int64_t min_z = cell(std::min(b.points[bj].z, b.points[bj + 1].z));
        const int64_t max_z = cell(std::max(b.points[bj].z, b.points[bj + 1].z));
        for (int64_t x = min_x; x <= max_x; ++x) {
            for (int64_t z = min_z; z <= max_z; ++z) {
                grid[gridKey(x, z)].push_back(bj);
            }
        }
    }

    std::vector<std::vector<size_t>> result(a.points.size() - 1);
    for (size_t ai = 0; ai + 1 < a.points.size(); ++ai) {
        const int64_t min_x = cell(
            std::min(a.points[ai].x, a.points[ai + 1].x) - tolerance);
        const int64_t max_x = cell(
            std::max(a.points[ai].x, a.points[ai + 1].x) + tolerance);
        const int64_t min_z = cell(
            std::min(a.points[ai].z, a.points[ai + 1].z) - tolerance);
        const int64_t max_z = cell(
            std::max(a.points[ai].z, a.points[ai + 1].z) + tolerance);
        std::vector<size_t>& candidates = result[ai];
        for (int64_t x = min_x; x <= max_x; ++x) {
            for (int64_t z = min_z; z <= max_z; ++z) {
                const auto found = grid.find(gridKey(x, z));
                if (found != grid.end()) {
                    candidates.insert(candidates.end(), found->second.begin(),
                                      found->second.end());
                }
            }
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()),
                         candidates.end());
    }
    return result;
}

// 生成稳定的线路小段 movement 标识，区分进入侧和离开侧。
std::string routeSegmentId(const std::string& line_id, size_t index,
                           const char* side) {
    return "LINE:" + line_id + ":SEG:" + std::to_string(index) + ":" + side;
}

// 在合并方向相反的共线候选时翻转线路相对方向。
RouteDirectionRelation flipped(RouteDirectionRelation relation) {
    return relation == RouteDirectionRelation::SAME
        ? RouteDirectionRelation::OPPOSITE : RouteDirectionRelation::SAME;
}

// 同时检查水平距离和高度差，用于合并代表同一空间位置的三维端点。
bool coordinatesNear(double ax, double ay, double az,
                     double bx, double by, double bz,
                     double horizontal_tolerance,
                     double vertical_tolerance) {
    return std::hypot(ax - bx, az - bz) <= horizontal_tolerance &&
           std::abs(ay - by) <= vertical_tolerance;
}

// 向共线段添加线路成员，并按 line_id 去重三线路两两分析产生的重复项。
void addCorridorMember(SharedCorridorDefinition& corridor,
                       const SharedCorridorMember& member) {
    auto existing = std::find_if(
        corridor.members.begin(), corridor.members.end(),
        [&](const SharedCorridorMember& value) {
            return value.line_id == member.line_id &&
                   value.occurrence_id == member.occurrence_id;
        });
    if (existing == corridor.members.end()) corridor.members.push_back(member);
}

void mergeCorridor(LineTopologyAnalysis& result,
                   SharedCorridorDefinition candidate,
                   SharedCorridorMember a_member,
                   SharedCorridorMember b_member,
                   double horizontal_tolerance,
                   double vertical_tolerance) {
    SharedCorridorDefinition* merged = nullptr;
    bool reversed_against_existing = false;
    for (SharedCorridorDefinition& existing : result.shared_corridors) {
        const bool same = coordinatesNear(
            existing.start_x, existing.start_y, existing.start_z,
            candidate.start_x, candidate.start_y, candidate.start_z,
            horizontal_tolerance, vertical_tolerance) && coordinatesNear(
            existing.end_x, existing.end_y, existing.end_z,
            candidate.end_x, candidate.end_y, candidate.end_z,
            horizontal_tolerance, vertical_tolerance);
        const bool reverse = coordinatesNear(
            existing.start_x, existing.start_y, existing.start_z,
            candidate.end_x, candidate.end_y, candidate.end_z,
            horizontal_tolerance, vertical_tolerance) && coordinatesNear(
            existing.end_x, existing.end_y, existing.end_z,
            candidate.start_x, candidate.start_y, candidate.start_z,
            horizontal_tolerance, vertical_tolerance);
        // 相同端点不能证明是同一次经过：环线、折返上下行可能回到同一位置。
        // 至少有一个共同成员作为轨迹依据；所有共同成员必须是同一索引区间和方向。
        auto compatible = [&](bool reversed) {
            bool common = false;
            for (const auto& incoming : {a_member, b_member}) {
                for (const auto& member : existing.members) {
                    if (incoming.line_id != member.line_id ||
                        incoming.occurrence_id != member.occurrence_id) continue;
                    common = true;
                    const auto relation = reversed
                        ? flipped(incoming.direction_relation) : incoming.direction_relation;
                    if (relation != member.direction_relation) return false;
                }
            }
            return common;
        };
        const bool merge_same = same && compatible(false);
        const bool merge_reverse = reverse && compatible(true);
        if (merge_same || merge_reverse) {
            merged = &existing;
            reversed_against_existing = !merge_same;
            break;
        }
    }

    if (!merged) {
        candidate.members.push_back(std::move(a_member));
        candidate.members.push_back(std::move(b_member));
        result.shared_corridors.push_back(std::move(candidate));
        return;
    }
    if (reversed_against_existing) {
        a_member.direction_relation = flipped(a_member.direction_relation);
        b_member.direction_relation = flipped(b_member.direction_relation);
    }
    addCorridorMember(*merged, a_member);
    addCorridorMember(*merged, b_member);
    merged->length = std::max(merged->length, candidate.length);
}

bool matchCorridorMembers(const SharedCorridorDefinition& lhs,
                          const SharedCorridorDefinition& rhs,
                          std::vector<size_t>* matches) {
    if (lhs.members.size() != rhs.members.size()) return false;
    matches->assign(lhs.members.size(), static_cast<size_t>(-1));
    std::vector<bool> used(rhs.members.size(), false);
    constexpr double kPositionEpsilon = 1e-5;
    for (size_t i = 0; i < lhs.members.size(); ++i) {
        const SharedCorridorMember& member = lhs.members[i];
        size_t best = static_cast<size_t>(-1);
        double best_gap = std::numeric_limits<double>::infinity();
        for (size_t j = 0; j < rhs.members.size(); ++j) {
            if (used[j]) continue;
            const SharedCorridorMember& value = rhs.members[j];
            if (value.line_id != member.line_id ||
                value.direction_relation != member.direction_relation) continue;
            const double gap = std::min(
                std::abs(member.end_position - value.start_position),
                std::abs(value.end_position - member.start_position));
            if (gap < best_gap) { best_gap = gap; best = j; }
        }
        if (best == static_cast<size_t>(-1) || best_gap > kPositionEpsilon)
            return false;
        used[best] = true;
        (*matches)[i] = best;
    }
    return true;
}

void absorbCorridor(SharedCorridorDefinition& target,
                    const SharedCorridorDefinition& source,
                    bool append_source,
                    const std::vector<size_t>& matches) {
    if (append_source) {
        target.end_x = source.end_x;
        target.end_y = source.end_y;
        target.end_z = source.end_z;
        if (!source.geometry.empty()) {
            const size_t first = !target.geometry.empty() &&
                coordinatesNear(target.geometry.back().x, target.geometry.back().y,
                    target.geometry.back().z, source.geometry.front().x,
                    source.geometry.front().y, source.geometry.front().z, 1e-6, 1e-6)
                ? 1 : 0;
            target.geometry.insert(target.geometry.end(),
                source.geometry.begin() + first, source.geometry.end());
        }
    } else {
        target.start_x = source.start_x;
        target.start_y = source.start_y;
        target.start_z = source.start_z;
        std::vector<LineRoutePoint> geometry = source.geometry;
        if (!geometry.empty() && !target.geometry.empty() &&
            coordinatesNear(geometry.back().x, geometry.back().y, geometry.back().z,
                target.geometry.front().x, target.geometry.front().y,
                target.geometry.front().z, 1e-6, 1e-6)) geometry.pop_back();
        geometry.insert(geometry.end(), target.geometry.begin(), target.geometry.end());
        target.geometry = std::move(geometry);
    }
    target.length += source.length;
    for (size_t i = 0; i < target.members.size(); ++i) {
        SharedCorridorMember& member = target.members[i];
        const SharedCorridorMember* found = &source.members[matches[i]];
        member.start_position = std::min(member.start_position,
                                         found->start_position);
        member.end_position = std::max(member.end_position,
                                       found->end_position);
        member.start_segment = std::min(member.start_segment,
                                        found->start_segment);
        member.end_segment = std::max(member.end_segment,
                                      found->end_segment);
        std::ostringstream occurrence;
        occurrence << std::setprecision(12) << member.line_id << '@'
                   << member.start_position << '-' << member.end_position;
        member.occurrence_id = occurrence.str();
    }
}

// 两两分析会因候选排序、曲线采样误差或三线路组合把一个连续走廊切成多个首尾相接
// 的资源。最终再按“成员集合相同 + 各线路索引连续 + 三维端点相接”做传递归并。
void consolidateAdjacentCorridors(LineTopologyAnalysis& result,
                                  double horizontal_tolerance,
                                  double vertical_tolerance) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < result.shared_corridors.size() && !changed; ++i) {
            for (size_t j = i + 1; j < result.shared_corridors.size(); ++j) {
                SharedCorridorDefinition& lhs = result.shared_corridors[i];
                const SharedCorridorDefinition& rhs = result.shared_corridors[j];
                const bool append = coordinatesNear(
                    lhs.end_x, lhs.end_y, lhs.end_z,
                    rhs.start_x, rhs.start_y, rhs.start_z,
                    horizontal_tolerance, vertical_tolerance);
                const bool prepend = coordinatesNear(
                    lhs.start_x, lhs.start_y, lhs.start_z,
                    rhs.end_x, rhs.end_y, rhs.end_z,
                    horizontal_tolerance, vertical_tolerance);
                if (!append && !prepend) continue;
                std::vector<size_t> matches;
                if (!matchCorridorMembers(lhs, rhs, &matches)) continue;
                absorbCorridor(lhs, rhs, append, matches);
                result.shared_corridors.erase(result.shared_corridors.begin() + j);
                changed = true;
                break;
            }
        }
    }

    // 合并后重新生成包含完整成员范围的稳定 ID。
    for (SharedCorridorDefinition& corridor : result.shared_corridors) {
        std::sort(corridor.members.begin(), corridor.members.end(),
                  [](const SharedCorridorMember& lhs,
                     const SharedCorridorMember& rhs) {
            return std::tie(lhs.line_id, lhs.occurrence_id,
                            lhs.start_position, lhs.end_position) <
                   std::tie(rhs.line_id, rhs.occurrence_id,
                            rhs.start_position, rhs.end_position);
        });
        std::ostringstream id;
        id << std::fixed << std::setprecision(6);
        id << "CORRIDOR";
        for (const SharedCorridorMember& member : corridor.members) {
            id << ':' << member.line_id << ':' << member.occurrence_id << ':'
               << member.start_position << '-' << member.end_position;
        }
        corridor.corridor_id = id.str();
    }
}

bool projectToRange(const LineRoute& route, double low, double high,
                    const LineRoutePoint& point, double* position,
                    double* distance) {
    double best = std::numeric_limits<double>::infinity();
    double best_position = low;
    const size_t first = static_cast<size_t>(std::floor(low));
    const size_t last = std::min(route.points.size() - 2,
        static_cast<size_t>(std::floor(std::max(low, high - 1e-9))));
    for (size_t segment = first; segment <= last; ++segment) {
        const auto& a = route.points[segment];
        const auto& b = route.points[segment + 1];
        const double dx = b.x - a.x, dz = b.z - a.z;
        const double denominator = dx * dx + dz * dz;
        double t = denominator <= 1e-12 ? 0.0
            : ((point.x - a.x) * dx + (point.z - a.z) * dz) / denominator;
        const double segment_low = segment == first ? low - first : 0.0;
        const double segment_high = segment == last ? high - last : 1.0;
        t = std::max(segment_low, std::min(segment_high, t));
        const auto projected = interpolate(a, b, t);
        const double horizontal = distance2d(projected, point);
        const double metric = std::hypot(horizontal, projected.y - point.y);
        if (metric < best) {
            best = metric;
            best_position = static_cast<double>(segment) + t;
        }
    }
    if (!std::isfinite(best)) return false;
    *position = best_position;
    *distance = best;
    return true;
}

double geometryLength(const std::vector<LineRoutePoint>& geometry) {
    double length = 0.0;
    for (size_t i = 1; i < geometry.size(); ++i)
        length += distance2d(geometry[i - 1], geometry[i]);
    return length;
}

// 在所有候选入口/出口处切开长走廊，再合并几何相同的原子片段。
// 这样每个资源内部的成员集合恒定，不会出现 A+B 长资源覆盖 A+B+C 子资源。
void atomizeOverlappingCorridors(LineTopologyAnalysis& result,
                                 const std::vector<LineRoute>& routes,
                                 double horizontal_tolerance,
                                 double vertical_tolerance) {
    std::unordered_map<std::string, const LineRoute*> by_id;
    for (const auto& route : routes) by_id[route.line_id] = &route;
    const auto original = result.shared_corridors;
    std::vector<SharedCorridorDefinition> pieces;
    for (const auto& corridor : original) {
        if (corridor.members.empty()) continue;
        const auto route_it = by_id.find(corridor.members.front().line_id);
        if (route_it == by_id.end()) continue;
        const LineRoute& anchor = *route_it->second;
        const auto& anchor_member = corridor.members.front();
        std::vector<double> cuts{anchor_member.start_position,
                                 anchor_member.end_position};
        for (const auto& other : original) {
            for (const LineRoutePoint& endpoint : {
                    LineRoutePoint{"", other.start_x, other.start_y, other.start_z},
                    LineRoutePoint{"", other.end_x, other.end_y, other.end_z}}) {
                double projected = 0.0, gap = 0.0;
                if (projectToRange(anchor, anchor_member.start_position,
                                   anchor_member.end_position, endpoint,
                                   &projected, &gap) &&
                    gap <= horizontal_tolerance &&
                    projected > anchor_member.start_position + 1e-7 &&
                    projected < anchor_member.end_position - 1e-7)
                    cuts.push_back(projected);
            }
        }
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end(), [](double a, double b) {
            return std::abs(a - b) <= 1e-7;
        }), cuts.end());
        for (size_t cut = 1; cut < cuts.size(); ++cut) {
            if (cuts[cut] - cuts[cut - 1] <= 1e-7) continue;
            SharedCorridorDefinition piece;
            piece.geometry = routeGeometry(anchor, cuts[cut - 1], cuts[cut]);
            piece.length = geometryLength(piece.geometry);
            if (piece.length <= 1e-6) continue;
            const auto& start = piece.geometry.front();
            const auto& end = piece.geometry.back();
            piece.start_x = start.x; piece.start_y = start.y; piece.start_z = start.z;
            piece.end_x = end.x; piece.end_y = end.y; piece.end_z = end.z;
            for (const auto& member : corridor.members) {
                const auto member_route_it = by_id.find(member.line_id);
                if (member_route_it == by_id.end()) continue;
                double p0 = 0.0, p1 = 0.0, gap0 = 0.0, gap1 = 0.0;
                if (!projectToRange(*member_route_it->second,
                                    member.start_position, member.end_position,
                                    start, &p0, &gap0) ||
                    !projectToRange(*member_route_it->second,
                                    member.start_position, member.end_position,
                                    end, &p1, &gap1) ||
                    gap0 > horizontal_tolerance || gap1 > horizontal_tolerance) continue;
                SharedCorridorMember clipped;
                clipped.line_id = member.line_id;
                setMemberBounds(clipped, *member_route_it->second, p0, p1);
                clipped.direction_relation = p1 >= p0
                    ? RouteDirectionRelation::SAME : RouteDirectionRelation::OPPOSITE;
                addCorridorMember(piece, clipped);
            }
            if (piece.members.size() >= 2) pieces.push_back(std::move(piece));
        }
    }

    result.shared_corridors.clear();
    for (auto& piece : pieces) {
        SharedCorridorDefinition* match = nullptr;
        bool reverse = false;
        for (auto& existing : result.shared_corridors) {
            const bool same = coordinatesNear(existing.start_x, existing.start_y,
                existing.start_z, piece.start_x, piece.start_y, piece.start_z,
                horizontal_tolerance, vertical_tolerance) &&
                coordinatesNear(existing.end_x, existing.end_y, existing.end_z,
                piece.end_x, piece.end_y, piece.end_z,
                horizontal_tolerance, vertical_tolerance);
            const bool reversed = coordinatesNear(existing.start_x, existing.start_y,
                existing.start_z, piece.end_x, piece.end_y, piece.end_z,
                horizontal_tolerance, vertical_tolerance) &&
                coordinatesNear(existing.end_x, existing.end_y, existing.end_z,
                piece.start_x, piece.start_y, piece.start_z,
                horizontal_tolerance, vertical_tolerance);
            if (same || reversed) { match = &existing; reverse = !same; break; }
        }
        if (!match) {
            result.shared_corridors.push_back(std::move(piece));
            continue;
        }
        for (auto member : piece.members) {
            if (reverse) member.direction_relation = flipped(member.direction_relation);
            addCorridorMember(*match, member);
        }
        match->length = std::max(match->length, piece.length);
    }
}

void filterShortSharedComponents(LineTopologyAnalysis& result,
                                 double minimum_length,
                                 double horizontal_tolerance,
                                 double vertical_tolerance) {
    const size_t count = result.shared_corridors.size();
    std::vector<size_t> parent(count);
    for (size_t i = 0; i < count; ++i) parent[i] = i;
    const auto root = [&](size_t value, const auto& self) -> size_t {
        return parent[value] == value ? value
            : parent[value] = self(parent[value], self);
    };
    const auto near = [&](const SharedCorridorDefinition& a,
                          const SharedCorridorDefinition& b) {
        return coordinatesNear(a.start_x, a.start_y, a.start_z,
                               b.start_x, b.start_y, b.start_z,
                               horizontal_tolerance, vertical_tolerance) ||
               coordinatesNear(a.start_x, a.start_y, a.start_z,
                               b.end_x, b.end_y, b.end_z,
                               horizontal_tolerance, vertical_tolerance) ||
               coordinatesNear(a.end_x, a.end_y, a.end_z,
                               b.start_x, b.start_y, b.start_z,
                               horizontal_tolerance, vertical_tolerance) ||
               coordinatesNear(a.end_x, a.end_y, a.end_z,
                               b.end_x, b.end_y, b.end_z,
                               horizontal_tolerance, vertical_tolerance);
    };
    for (size_t i = 0; i < count; ++i) for (size_t j = i + 1; j < count; ++j) {
        if (!near(result.shared_corridors[i], result.shared_corridors[j])) continue;
        size_t ri = root(i, root), rj = root(j, root);
        if (ri != rj) parent[rj] = ri;
    }
    std::unordered_map<size_t, double> lengths;
    for (size_t i = 0; i < count; ++i)
        lengths[root(i, root)] += result.shared_corridors[i].length;
    std::vector<SharedCorridorDefinition> kept;
    for (size_t i = 0; i < count; ++i)
        if (lengths[root(i, root)] + 1e-7 >= minimum_length)
            kept.push_back(std::move(result.shared_corridors[i]));
    result.shared_corridors = std::move(kept);
}

// 在 x/z 平面求两条有限线段交点及各自参数 t/u；平行线交由共线流程处理。
bool segmentIntersection(const LineRoutePoint& a0, const LineRoutePoint& a1,
                         const LineRoutePoint& b0, const LineRoutePoint& b1,
                         double& t, double& u, double& x, double& z) {
    const double rx = a1.x - a0.x;
    const double rz = a1.z - a0.z;
    const double sx = b1.x - b0.x;
    const double sz = b1.z - b0.z;
    const double denominator = rx * sz - rz * sx;
    // 平行或共线情况由共线段识别流程处理，这里只处理孤立交点。
    if (std::abs(denominator) < 1e-9) return false;

    const double qpx = b0.x - a0.x;
    const double qpz = b0.z - a0.z;
    t = (qpx * sz - qpz * sx) / denominator;
    u = (qpx * rz - qpz * rx) / denominator;
    constexpr double epsilon = 1e-8;
    if (t < -epsilon || t > 1.0 + epsilon ||
        u < -epsilon || u > 1.0 + epsilon) {
        return false;
    }
    t = std::max(0.0, std::min(1.0, t));
    u = std::max(0.0, std::min(1.0, u));
    x = a0.x + t * rx;
    z = a0.z + t * rz;
    return true;
}

} // namespace

// 校验所有几何阈值为有限正数，避免分析过程出现无意义比较。
LineRouteAnalyzer::LineRouteAnalyzer(LineRouteAnalyzerConfig config)
    : config_(config) {
    if (!std::isfinite(config_.point_merge_tolerance) ||
        !std::isfinite(config_.shared_track_tolerance) ||
        !std::isfinite(config_.vertical_separation_tolerance) ||
        !std::isfinite(config_.shared_corridor_min_length) ||
        !std::isfinite(config_.intersection_min_angle_degrees) ||
        config_.point_merge_tolerance <= 0.0 ||
        config_.shared_track_tolerance <= 0.0 ||
        config_.shared_track_tolerance > config_.point_merge_tolerance ||
        config_.vertical_separation_tolerance <= 0.0 ||
        config_.shared_corridor_min_length <= 0.0 ||
        config_.intersection_min_angle_degrees <= 0.0 ||
        config_.intersection_min_angle_degrees > 90.0) {
        throw std::invalid_argument("line route analyzer limits must be positive");
    }
}

// 对有效线路两两扫描：第一遍合并连续重合小段，第二遍排除共线后寻找
// 孤立三维交点，并为交点两侧线路生成互斥 movement。
LineTopologyAnalysis LineRouteAnalyzer::analyze(
    const std::vector<LineRoute>& input_routes) const {
    std::vector<LineRoute> routes;
    for (const LineRoute& route : input_routes) {
        if (!route.line_id.empty() && route.points.size() >= 2) routes.push_back(route);
    }
    // 固定线路遍历顺序，使自动生成的 corridor/intersection ID 可复现。
    std::sort(routes.begin(), routes.end(), [](const LineRoute& lhs,
                                                const LineRoute& rhs) {
        return lhs.line_id < rhs.line_id;
    });

    LineTopologyAnalysis result;
    for (size_t a_index = 0; a_index < routes.size(); ++a_index) {
        for (size_t b_index = a_index; b_index < routes.size(); ++b_index) {
            const LineRoute& a = routes[a_index];
            const LineRoute& b = routes[b_index];
            const bool self_pair = a_index == b_index;
            if (!self_pair && !a.transport_type.empty() && !b.transport_type.empty() &&
                a.transport_type != b.transport_type) continue;
            std::unordered_set<uint64_t> shared_pairs;
            const std::vector<std::vector<size_t>> candidates =
                spatialCandidates(a, b, config_.point_merge_tolerance);

            // 第一遍：对小段做近似平行、垂距、高度和投影重叠匹配。匹配的是
            // 连续轨迹而不是采样端点，因此两条线路采用不同采样密度也能对齐。
            struct OverlapFragment {
                size_t a_segment = 0;
                size_t b_segment = 0;
                SegmentOverlap overlap;
                double a_order_start = 0.0;
                double a_order_end = 0.0;
            };
            std::vector<OverlapFragment> fragments;
            for (size_t ai = 0; ai + 1 < a.points.size(); ++ai) {
                for (size_t bj : candidates[ai]) {
                    if (self_pair && bj <= ai) continue;
                    SegmentOverlap overlap;
                    if (!segmentOverlap(
                            a.points[ai], a.points[ai + 1],
                            b.points[bj], b.points[bj + 1],
                            config_.shared_track_tolerance,
                            config_.vertical_separation_tolerance,
                            config_.intersection_min_angle_degrees, &overlap)) continue;
                    shared_pairs.insert(segmentPairKey(ai, bj));
                    OverlapFragment fragment;
                    fragment.a_segment = ai;
                    fragment.b_segment = bj;
                    fragment.overlap = overlap;
                    fragment.a_order_start = static_cast<double>(ai) + overlap.a_start_t;
                    fragment.a_order_end = static_cast<double>(ai) + overlap.a_end_t;
                    fragments.push_back(std::move(fragment));
                }
            }

            // 连续匹配是 (A segment,B segment) 网格里的连通分量。只按 A 排序会让
            // 环线的多次 B 经过交错，进而把每次完整经过切成短片段。
            std::vector<size_t> parent(fragments.size());
            for (size_t i = 0; i < parent.size(); ++i) parent[i] = i;
            const auto root = [&](size_t value, const auto& self) -> size_t {
                return parent[value] == value ? value
                    : parent[value] = self(parent[value], self);
            };
            for (size_t i = 0; i < fragments.size(); ++i) {
                for (size_t j = i + 1; j < fragments.size(); ++j) {
                    if (fragments[i].overlap.orientation != fragments[j].overlap.orientation)
                        continue;
                    const size_t a_gap = fragments[i].a_segment > fragments[j].a_segment
                        ? fragments[i].a_segment - fragments[j].a_segment
                        : fragments[j].a_segment - fragments[i].a_segment;
                    const size_t b_gap = fragments[i].b_segment > fragments[j].b_segment
                        ? fragments[i].b_segment - fragments[j].b_segment
                        : fragments[j].b_segment - fragments[i].b_segment;
                    if (a_gap > 1 || b_gap > 1) continue;
                    const bool near = coordinatesNear(
                        fragments[i].overlap.end.x, fragments[i].overlap.end.y,
                        fragments[i].overlap.end.z, fragments[j].overlap.start.x,
                        fragments[j].overlap.start.y, fragments[j].overlap.start.z,
                        config_.point_merge_tolerance,
                        config_.vertical_separation_tolerance) ||
                        coordinatesNear(
                        fragments[j].overlap.end.x, fragments[j].overlap.end.y,
                        fragments[j].overlap.end.z, fragments[i].overlap.start.x,
                        fragments[i].overlap.start.y, fragments[i].overlap.start.z,
                        config_.point_merge_tolerance,
                        config_.vertical_separation_tolerance) ||
                        (fragments[i].a_order_start <= fragments[j].a_order_end &&
                         fragments[j].a_order_start <= fragments[i].a_order_end);
                    if (!near) continue;
                    size_t ri = root(i, root), rj = root(j, root);
                    if (ri != rj) parent[rj] = ri;
                }
            }
            std::map<size_t, std::vector<size_t>> components;
            for (size_t i = 0; i < fragments.size(); ++i)
                components[root(i, root)].push_back(i);

            for (auto& component : components) {
                auto& indices = component.second;
                std::sort(indices.begin(), indices.end(), [&](size_t lhs, size_t rhs) {
                    return fragments[lhs].a_order_start < fragments[rhs].a_order_start;
                });
                const int orientation = fragments[indices.front()].overlap.orientation;
                double a_start = fragments[indices.front()].a_order_start;
                double a_end = fragments[indices.front()].a_order_end;
                double b_start = static_cast<double>(fragments[indices.front()].b_segment) +
                                 fragments[indices.front()].overlap.b_start_t;
                double b_end = static_cast<double>(fragments[indices.front()].b_segment) +
                               fragments[indices.front()].overlap.b_end_t;
                std::map<size_t, std::vector<std::pair<double, double>>> coverage;
                for (size_t index : indices) {
                    const auto& fragment = fragments[index];
                    a_start = std::min(a_start, fragment.a_order_start);
                    a_end = std::max(a_end, fragment.a_order_end);
                    const double bp0 = static_cast<double>(fragment.b_segment) +
                                       fragment.overlap.b_start_t;
                    const double bp1 = static_cast<double>(fragment.b_segment) +
                                       fragment.overlap.b_end_t;
                    b_start = std::min(b_start, std::min(bp0, bp1));
                    b_end = std::max(b_end, std::max(bp0, bp1));
                    coverage[fragment.a_segment].push_back(
                        {fragment.overlap.a_start_t, fragment.overlap.a_end_t});
                }
                double length = 0.0;
                for (auto& item : coverage) {
                    auto& intervals = item.second;
                    std::sort(intervals.begin(), intervals.end());
                    double low = intervals.front().first, high = intervals.front().second;
                    for (size_t i = 1; i < intervals.size(); ++i) {
                        if (intervals[i].first <= high + 1e-8)
                            high = std::max(high, intervals[i].second);
                        else {
                            length += (high - low) * segmentLength(a, item.first);
                            low = intervals[i].first; high = intervals[i].second;
                        }
                    }
                    length += (high - low) * segmentLength(a, item.first);
                }
                if (length <= 1e-6) continue;
                SharedCorridorDefinition candidate;
                candidate.geometry = routeGeometry(a, a_start, a_end);
                const auto& start = candidate.geometry.front();
                const auto& end = candidate.geometry.back();
                candidate.start_x = start.x; candidate.start_y = start.y;
                candidate.start_z = start.z; candidate.end_x = end.x;
                candidate.end_y = end.y; candidate.end_z = end.z;
                candidate.length = length;
                SharedCorridorMember a_member, b_member;
                a_member.line_id = a.line_id;
                setMemberBounds(a_member, a, a_start, a_end);
                a_member.direction_relation = RouteDirectionRelation::SAME;
                b_member.line_id = b.line_id;
                setMemberBounds(b_member, b, b_start, b_end);
                b_member.direction_relation = orientation == 1
                    ? RouteDirectionRelation::SAME : RouteDirectionRelation::OPPOSITE;
                candidate.corridor_id = "PAIR:" + a_member.occurrence_id + ':' +
                                        b_member.occurrence_id;
                mergeCorridor(result, std::move(candidate),
                              std::move(a_member), std::move(b_member),
                              config_.shared_track_tolerance,
                              config_.vertical_separation_tolerance);
            }

            // 第二遍：排除已知共线小段后，寻找孤立的线路折线交点。
            for (size_t ai = 0; ai + 1 < a.points.size(); ++ai) {
                for (size_t bj : candidates[ai]) {
                    if (self_pair && (bj <= ai || bj == ai + 1 ||
                        (ai == 0 && bj + 2 == a.points.size()))) continue;
                    if (shared_pairs.count(segmentPairKey(ai, bj)) != 0) continue;
                    if (!segmentBoundsNear(
                            a.points[ai], a.points[ai + 1],
                            b.points[bj], b.points[bj + 1], 0.0)) continue;
                    // 近似平行的小段属于同轨、并轨或分岔关系，不应因为数值误差
                    // 在公共端点生成成百上千个普通交叉点。
                    if (segmentAcuteAngleDegrees(
                            a.points[ai], a.points[ai + 1],
                            b.points[bj], b.points[bj + 1]) <
                        config_.intersection_min_angle_degrees) continue;
                    double t = 0.0, u = 0.0, x = 0.0, z = 0.0;
                    if (!segmentIntersection(a.points[ai], a.points[ai + 1],
                                             b.points[bj], b.points[bj + 1],
                                             t, u, x, z)) {
                        continue;
                    }

                    // 分别插值两条线路在平面交点处的高度；差值过大说明
                    // 是高架/地面或隧道投影交叉，不生成调度冲突。
                    const double height_a = a.points[ai].y +
                        t * (a.points[ai + 1].y - a.points[ai].y);
                    const double height_b = b.points[bj].y +
                        u * (b.points[bj + 1].y - b.points[bj].y);
                    if (std::abs(height_a - height_b) >
                        config_.vertical_separation_tolerance) {
                        continue;
                    }

                    bool duplicate = false;
                    for (const LineIntersectionDefinition& existing : result.intersections) {
                        if (existing.line_a_id == a.line_id &&
                            existing.line_b_id == b.line_id &&
                            std::hypot(existing.x - x, existing.z - z) <=
                                config_.point_merge_tolerance &&
                            std::abs(existing.y - 0.5 * (height_a + height_b)) <=
                                config_.vertical_separation_tolerance) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (duplicate) continue;

                    LineIntersectionDefinition intersection;
                    intersection.intersection_id = "INTERSECTION:" + a.line_id + ':' +
                        std::to_string(ai) + ':' + b.line_id + ':' + std::to_string(bj);
                    intersection.x = x;
                    intersection.z = z;
                    intersection.y = 0.5 * (height_a + height_b);
                    intersection.line_a_id = a.line_id;
                    intersection.line_b_id = b.line_id;
                    intersection.line_a_segment = ai;
                    intersection.line_b_segment = bj;
                    intersection.line_a_movement_id = intersection.intersection_id +
                        ":MOVE:" + a.line_id + ':' + std::to_string(ai);
                    intersection.line_b_movement_id = intersection.intersection_id +
                        ":MOVE:" + b.line_id + ':' + std::to_string(bj);

                    // 第一版采用保守策略：同一交点的两条线路 movement 互斥。
                    LineIntersectionMovement a_movement;
                    a_movement.movement_id = intersection.line_a_movement_id;
                    a_movement.intersection_id = intersection.intersection_id;
                    a_movement.line_id = a.line_id;
                    a_movement.incoming_route_segment_id = routeSegmentId(a.line_id, ai, "IN");
                    a_movement.outgoing_route_segment_id = routeSegmentId(a.line_id, ai, "OUT");
                    a_movement.conflict_movement_ids = {intersection.line_b_movement_id};

                    LineIntersectionMovement b_movement;
                    b_movement.movement_id = intersection.line_b_movement_id;
                    b_movement.intersection_id = intersection.intersection_id;
                    b_movement.line_id = b.line_id;
                    b_movement.incoming_route_segment_id = routeSegmentId(b.line_id, bj, "IN");
                    b_movement.outgoing_route_segment_id = routeSegmentId(b.line_id, bj, "OUT");
                    b_movement.conflict_movement_ids = {intersection.line_a_movement_id};

                    result.intersections.push_back(intersection);
                    result.movements.push_back(std::move(a_movement));
                    result.movements.push_back(std::move(b_movement));
                }
            }
        }
    }
    atomizeOverlappingCorridors(result, routes, config_.shared_track_tolerance,
                                config_.vertical_separation_tolerance);
    consolidateAdjacentCorridors(result, config_.shared_track_tolerance,
                                 config_.vertical_separation_tolerance);
    filterShortSharedComponents(result, config_.shared_corridor_min_length,
                                config_.shared_track_tolerance,
                                config_.vertical_separation_tolerance);
    return result;
}
