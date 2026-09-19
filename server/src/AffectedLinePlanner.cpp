#include "AffectedLinePlanner.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
// 构造内部复合键，避免同一列车经过多个共线段时预测结果相互覆盖。
std::string planKey(const std::string& train_id,
                    const std::string& resource_id) {
    return train_id + "\n" + resource_id;
}

// 把几何共线段和方向组合成运行时资源 ID。
std::string resourceId(const std::string& corridor_id,
                       CorridorDirection direction) {
    return corridor_id + (direction == CorridorDirection::FORWARD
        ? ":FORWARD" : ":REVERSE");
}

// 将“线路索引增减方向”转换成“相对共线段标准方向”。
CorridorDirection corridorDirection(const CorridorTraversal& traversal,
                                    RouteTravelDirection travel) {
    const bool increasing = travel == RouteTravelDirection::FORWARD;
    const bool route_matches =
        traversal.direction_relation == RouteDirectionRelation::SAME;
    return increasing == route_matches
        ? CorridorDirection::FORWARD : CorridorDirection::REVERSE;
}

// 计算一个线路 segment 在 x/z 平面的长度，供沿线累计距离使用。
double segmentLength(const LineRoute& route, size_t index) {
    const auto& a = route.points[index];
    const auto& b = route.points[index + 1];
    return std::hypot(b.x - a.x, b.z - a.z);
}

// 计算列车沿当前运行方向到入口的折线距离。当前线路模型按线性索引处理；
// 环线应由 Mod 把首点重复在末尾，并保证 route_index 与这套顺序一致。
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

// 对 uint64_t 时间做饱和加法，极端 ETA 不发生无符号回绕。
uint64_t addSaturated(uint64_t lhs, uint64_t rhs) {
    return rhs > std::numeric_limits<uint64_t>::max() - lhs
        ? std::numeric_limits<uint64_t>::max() : lhs + rhs;
}
} // namespace

// 从已生成结果中按复合键取一份预测，供入口候选附加全局软分数。
std::optional<PlannedTrainArrival> RollingPlanResult::find(
    const std::string& train_id,
    const std::string& corridor_resource_id) const {
    auto it = by_train_resource.find(planKey(train_id, corridor_resource_id));
    if (it == by_train_resource.end()) return std::nullopt;
    return it->second;
}

// 拒绝会导致除零、无限 ETA 或无效换序窗口的配置。
AffectedLinePlanner::AffectedLinePlanner(AffectedLinePlannerConfig config)
    : config_(config) {
    if (config_.planning_horizon_ms == 0 ||
        config_.reorder_tolerance_ms == 0 ||
        config_.planned_minimum_spacing_ms == 0 ||
        !std::isfinite(config_.minimum_prediction_speed) ||
        config_.minimum_prediction_speed <= 0.0 ||
        !std::isfinite(config_.same_line_consecutive_penalty) ||
        config_.same_line_consecutive_penalty < 0.0) {
        throw std::invalid_argument("invalid affected-line planner config");
    }
}

// 完成一轮无副作用规划：建立到达预测、计算同线车群分布，再在 ETA 容差
// 窗口内生成有限换序结果。所有硬预约仍由后续 DispatchManager 执行。
RollingPlanResult AffectedLinePlanner::plan(
    const std::vector<MonitoredTrain>& trains,
    const LineTopologyRegistry& topology,
    uint64_t simulation_now_ms) const {
    RollingPlanResult result;

    // 第一阶段扫描所有共线相关线路列车。普通、不含共线段的线路不会产生
    // PlannedTrainArrival，但仍保留在 TrainMonitor 的全量监控表中。
    for (const MonitoredTrain& monitored : trains) {
        const TrainTelemetry& train = monitored.telemetry;
        const auto route = topology.route(train.line_id);
        if (!route.has_value()) continue;
        const auto traversals = topology.corridorsForLine(train.line_id);
        if (traversals.empty()) continue;

        const double prediction_speed = std::max(
            std::abs(train.speed), config_.minimum_prediction_speed);
        for (const CorridorTraversal& traversal : traversals) {
            double distance = 0.0;
            if (!distanceToEntry(*route, traversal, train, &distance)) continue;
            const double travel_ms_double = distance / prediction_speed * 1000.0;
            const uint64_t travel_ms = travel_ms_double >=
                    static_cast<double>(std::numeric_limits<uint64_t>::max())
                ? std::numeric_limits<uint64_t>::max()
                : static_cast<uint64_t>(std::ceil(travel_ms_double));

            PlannedTrainArrival planned;
            planned.train_id = train.train_id;
            planned.line_id = train.line_id;
            planned.corridor_resource_id = resourceId(
                traversal.corridor_id,
                corridorDirection(traversal, train.travel_direction));
            planned.distance_to_entry = distance;
            planned.estimated_arrival_simulation_ms =
                addSaturated(simulation_now_ms, travel_ms);
            planned.target_headway_ms = route->target_headway_ms;
            planned.within_planning_horizon =
                travel_ms <= config_.planning_horizon_ms;

            CorridorRollingPlan& plan =
                result.plans_by_resource[planned.corridor_resource_id];
            plan.corridor_resource_id = planned.corridor_resource_id;
            plan.generated_at_simulation_ms = simulation_now_ms;
            plan.trains.push_back(std::move(planned));
        }
    }

    for (auto& plan_pair : result.plans_by_resource) {
        CorridorRollingPlan& plan = plan_pair.second;
        auto& states = plan.trains;
        std::sort(states.begin(), states.end(),
                  [](const PlannedTrainArrival& lhs,
                     const PlannedTrainArrival& rhs) {
            if (lhs.estimated_arrival_simulation_ms !=
                rhs.estimated_arrival_simulation_ms) {
                return lhs.estimated_arrival_simulation_ms <
                       rhs.estimated_arrival_simulation_ms;
            }
            return lhs.train_id < rhs.train_id;
        });

        // 同线路相邻列车过近时：领头车获得正值，跟随车获得负值。
        // 这样全局规划会倾向先放走领头车，而不是继续把车群压在一起。
        std::unordered_map<std::string, std::vector<size_t>> indices_by_line;
        for (size_t i = 0; i < states.size(); ++i)
            indices_by_line[states[i].line_id].push_back(i);
        for (const auto& line_pair : indices_by_line) {
            const auto& indices = line_pair.second;
            for (size_t position = 0; position < indices.size(); ++position) {
                PlannedTrainArrival& current = states[indices[position]];
                double need = 0.0;
                if (position > 0) {
                    const auto& previous = states[indices[position - 1]];
                    const uint64_t gap = current.estimated_arrival_simulation_ms -
                                         previous.estimated_arrival_simulation_ms;
                    if (gap < current.target_headway_ms)
                        need -= 1.0 - static_cast<double>(gap) /
                                      current.target_headway_ms;
                }
                if (position + 1 < indices.size()) {
                    const auto& next = states[indices[position + 1]];
                    const uint64_t gap = next.estimated_arrival_simulation_ms -
                                         current.estimated_arrival_simulation_ms;
                    if (gap < current.target_headway_ms)
                        need += 1.0 - static_cast<double>(gap) /
                                      current.target_headway_ms;
                }
                current.line_distribution_need =
                    std::max(-1.0, std::min(1.0, need));
            }
        }

        // 在最早 ETA 附近的容差窗口中滚动选下一辆；既尊重到达先后，也允许
        // 为改善班次分布做有限换序。这里只生成软建议，不做资源预约。
        std::vector<bool> selected(states.size(), false);
        std::string last_line;
        uint64_t last_planned_entry = 0;
        size_t rank = 0;
        while (true) {
            size_t earliest = states.size();
            for (size_t i = 0; i < states.size(); ++i) {
                if (!selected[i] && states[i].within_planning_horizon) {
                    earliest = i;
                    break;
                }
            }
            if (earliest == states.size()) break;
            const uint64_t cutoff = addSaturated(
                states[earliest].estimated_arrival_simulation_ms,
                config_.reorder_tolerance_ms);
            size_t best = earliest;
            double best_score = -std::numeric_limits<double>::infinity();
            for (size_t i = earliest; i < states.size(); ++i) {
                if (states[i].estimated_arrival_simulation_ms > cutoff) break;
                if (selected[i] || !states[i].within_planning_horizon) continue;
                const uint64_t until_arrival =
                    states[i].estimated_arrival_simulation_ms - simulation_now_ms;
                const double arrival_urgency = 1.0 - std::min(
                    1.0, static_cast<double>(until_arrival) /
                         config_.planning_horizon_ms);
                double score = 0.6 * arrival_urgency +
                               0.4 * states[i].line_distribution_need;
                if (!last_line.empty() && states[i].line_id == last_line)
                    score -= config_.same_line_consecutive_penalty;
                if (score > best_score ||
                    (score == best_score && states[i].train_id < states[best].train_id)) {
                    best = i;
                    best_score = score;
                }
            }

            selected[best] = true;
            states[best].suggested_rank = rank++;
            const uint64_t earliest_slot = last_planned_entry == 0
                ? states[best].estimated_arrival_simulation_ms
                : addSaturated(last_planned_entry,
                               config_.planned_minimum_spacing_ms);
            states[best].planned_entry_simulation_ms = std::max(
                states[best].estimated_arrival_simulation_ms, earliest_slot);
            last_planned_entry = states[best].planned_entry_simulation_ms;
            last_line = states[best].line_id;
            states[best].global_priority =
                1.0 / static_cast<double>(states[best].suggested_rank + 1) +
                0.5 * states[best].line_distribution_need;
        }

        for (const PlannedTrainArrival& state : states)
            result.by_train_resource[planKey(
                state.train_id, state.corridor_resource_id)] = state;
        std::stable_sort(states.begin(), states.end(),
                         [](const PlannedTrainArrival& lhs,
                            const PlannedTrainArrival& rhs) {
            if (lhs.within_planning_horizon != rhs.within_planning_horizon)
                return lhs.within_planning_horizon;
            if (lhs.suggested_rank != rhs.suggested_rank)
                return lhs.suggested_rank < rhs.suggested_rank;
            return lhs.estimated_arrival_simulation_ms <
                   rhs.estimated_arrival_simulation_ms;
        });
    }
    return result;
}
