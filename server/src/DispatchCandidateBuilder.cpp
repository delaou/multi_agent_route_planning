#include "DispatchCandidateBuilder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace {
// 计算线路小段的水平长度，控制范围采用沿线路距离而不是端点直线距离。
double segmentLength(const LineRoute& route, size_t index) {
    const LineRoutePoint& a = route.points[index];
    const LineRoutePoint& b = route.points[index + 1];
    return std::hypot(b.x - a.x, b.z - a.z);
}

// 把列车在线路上的增减方向映射为共线段的正/反向资源。
CorridorDirection corridorDirection(const CorridorTraversal& traversal,
                                    RouteTravelDirection travel) {
    const bool route_increasing = travel == RouteTravelDirection::FORWARD;
    const bool increasing_matches_corridor =
        traversal.direction_relation == RouteDirectionRelation::SAME;
    return route_increasing == increasing_matches_corridor
        ? CorridorDirection::FORWARD : CorridorDirection::REVERSE;
}

// 使用线路折线的累计距离，而不是“相差几个采样点”，因此改变采样密度不会
// 改变控制区的实际长度。返回 false 表示列车已在入口另一侧或索引无效。
bool distanceToEntry(const LineRoute& route,
                     const CorridorTraversal& traversal,
                     const TrainTelemetry& train,
                     double* distance) {
    if (route.points.size() < 2) return false;
    const double maximum = static_cast<double>(route.points.size() - 1);
    const double current = std::max(0.0, std::min(maximum, train.route_position));
    const double entry = train.travel_direction == RouteTravelDirection::FORWARD
        ? traversal.low_position : traversal.high_position;
    double delta = train.travel_direction == RouteTravelDirection::FORWARD
        ? entry - current : current - entry;
    if (delta < -1e-6 && route.is_loop) delta += maximum;
    if (delta < -1e-6) return false;
    double result = 0.0;
    double position = current;
    while (delta > 1e-9) {
        size_t index = std::min(route.points.size() - 2,
            static_cast<size_t>(std::floor(position)));
        double fraction = position - std::floor(position);
        if (train.travel_direction == RouteTravelDirection::REVERSE &&
            fraction <= 1e-9 && position > 0.0) {
            index = static_cast<size_t>(std::floor(position)) - 1;
            fraction = 1.0;
        }
        const double step = std::min(delta, train.travel_direction == RouteTravelDirection::FORWARD
            ? 1.0 - fraction : (fraction > 1e-9 ? fraction : 1.0));
        result += step * segmentLength(route, index);
        delta -= step;
        position += train.travel_direction == RouteTravelDirection::FORWARD ? step : -step;
        if (position >= maximum - 1e-9 && delta > 1e-9) position = 0.0;
        if (position <= 1e-9 && delta > 1e-9 &&
            train.travel_direction == RouteTravelDirection::REVERSE) position = maximum;
    }
    *distance = result;
    return true;
}

// 判断某交叉 movement 是否位于“列车当前位置到当前共线出口”之间。
bool liesAheadThroughCorridor(const IntersectionTraversal& intersection,
                              const CorridorTraversal& corridor,
                              const TrainTelemetry& train) {
    if (train.travel_direction == RouteTravelDirection::FORWARD) {
        return intersection.segment_index >= train.route_index &&
               intersection.segment_index <= corridor.high_segment;
    }
    return intersection.segment_index <= train.route_index &&
           intersection.segment_index >= corridor.low_segment;
}

// 为首次进入候选区的计时生成复合键；同一车在不同资源分别计时。
std::string waitingKey(const std::string& train_id,
                       const std::string& resource_id) {
    return train_id + "\n" + resource_id;
}
} // namespace

// 校验两级控制范围：实时竞争区必须包含在外层接近区内。
DispatchCandidateBuilder::DispatchCandidateBuilder(
    DispatchCandidateBuilderConfig config)
    : config_(config) {
    if (!std::isfinite(config_.approach_distance) ||
        !std::isfinite(config_.control_point_distance) ||
        config_.approach_distance <= 0.0 ||
        config_.control_point_distance < 0.0 ||
        config_.control_point_distance > config_.approach_distance) {
        throw std::invalid_argument("invalid dispatch candidate distances");
    }
}

// 生成全局一致的共线方向资源 ID，规划器、管理器和 HTTP 都使用该格式。
std::string DispatchCandidateBuilder::resourceId(
    const std::string& corridor_id, CorridorDirection direction) {
    return corridor_id + (direction == CorridorDirection::FORWARD
        ? ":FORWARD" : ":REVERSE");
}

// 每辆车只选择当前方向上最近的未来共线段，避免同一列车同时收到多个
// RELEASE；只有进入 control_point_distance 后才真正生成候选。
CandidateBuildResult DispatchCandidateBuilder::build(
    const std::vector<MonitoredTrain>& trains,
    const LineTopologyRegistry& topology,
    const RollingPlanResult* rolling_plan) {
    CandidateBuildResult result;
    std::unordered_set<std::string> active_waiting_keys;
    std::lock_guard<std::mutex> lock(mutex_);

    for (const MonitoredTrain& monitored : trains) {
        const TrainTelemetry& train = monitored.telemetry;
        result.phase_by_train[train.train_id] = TrainDispatchPhase::MONITORED;

        // 已下发命令或已经进入资源的列车由 ACK/clear 事件驱动，不能因新一帧
        // route_index 抖动而重新加入候选集合。
        if (monitored.phase == TrainDispatchPhase::RELEASE_PENDING ||
            monitored.phase == TrainDispatchPhase::INSIDE_RESOURCE) {
            result.phase_by_train[train.train_id] = monitored.phase;
            continue;
        }

        const auto route = topology.route(train.line_id);
        if (!route.has_value()) continue;
        const auto traversals = topology.corridorsForLine(train.line_id);
        const auto intersections = topology.intersectionsForLine(train.line_id);

        const CorridorTraversal* nearest = nullptr;
        double nearest_distance = std::numeric_limits<double>::infinity();
        for (const CorridorTraversal& traversal : traversals) {
            double distance = 0.0;
            if (distanceToEntry(*route, traversal, train, &distance) &&
                distance <= config_.approach_distance &&
                distance < nearest_distance) {
                nearest = &traversal;
                nearest_distance = distance;
            }
        }
        if (!nearest) continue;

        result.phase_by_train[train.train_id] = TrainDispatchPhase::APPROACHING;
        if (nearest_distance > config_.control_point_distance) continue;

        const CorridorDirection direction = corridorDirection(*nearest,
                                                               train.travel_direction);
        const std::string resource_id = resourceId(nearest->corridor_id, direction);
        const std::string key = waitingKey(train.train_id, resource_id);
        active_waiting_keys.insert(key);
        WaitingRecord& waiting = waiting_by_train_resource_[key];
        if (waiting.arrived_at_simulation_ms == 0 ||
            train.simulation_time_ms < waiting.arrived_at_simulation_ms) {
            // 时间回退说明游戏重新加载，以新时间重新开始计算等待时长。
            waiting.arrived_at_simulation_ms = train.simulation_time_ms;
        }
        waiting.last_seen_wall_ms = train.received_at_wall_ms;

        DispatchCandidate candidate;
        candidate.train_id = train.train_id;
        candidate.line_id = train.line_id;
        candidate.corridor_direction = direction;
        candidate.corridor_resource_id = resource_id;
        candidate.route_index = train.route_index;
        candidate.route_position = train.route_position;
        candidate.arrived_at_simulation_ms = waiting.arrived_at_simulation_ms;
        candidate.received_at_wall_ms = train.received_at_wall_ms;
        if (rolling_plan) {
            const auto planned = rolling_plan->find(train.train_id, resource_id);
            if (planned.has_value()) {
                candidate.predicted_arrival_simulation_ms =
                    planned->estimated_arrival_simulation_ms;
                candidate.rolling_plan_rank = planned->suggested_rank;
                candidate.global_plan_priority = planned->global_priority;
            }
        }
        for (const IntersectionTraversal& intersection : intersections) {
            if (liesAheadThroughCorridor(intersection, *nearest, train)) {
                candidate.required_intersection_movement_ids.push_back(
                    intersection.movement_id);
            }
        }
        result.candidates_by_resource[resource_id].push_back(std::move(candidate));
        result.phase_by_train[train.train_id] = TrainDispatchPhase::WAITING_PERMISSION;
    }

    // 列车驶离控制区或换线后丢弃旧计时；仍在区内的 key 保持不变。
    for (auto it = waiting_by_train_resource_.begin();
         it != waiting_by_train_resource_.end();) {
        if (active_waiting_keys.count(it->first) == 0)
            it = waiting_by_train_resource_.erase(it);
        else
            ++it;
    }
    return result;
}

// 删除 train_id 对应的所有复合键，让它下次进入时重新开始等待计时。
void DispatchCandidateBuilder::forgetTrain(const std::string& train_id) {
    const std::string prefix = train_id + "\n";
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = waiting_by_train_resource_.begin();
         it != waiting_by_train_resource_.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0)
            it = waiting_by_train_resource_.erase(it);
        else
            ++it;
    }
}

// 在线路拓扑整体变化时清空全部旧资源等待记录。
void DispatchCandidateBuilder::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    waiting_by_train_resource_.clear();
}
