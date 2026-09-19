#include "DynamicPriorityPolicy.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
// 安全计算时间差；游戏重载导致时间回退时返回 0，避免 uint64_t 下溢。
uint64_t elapsedSince(uint64_t now_ms, uint64_t then_ms) {
    // 游戏重新加载或时间回退时避免无符号整数下溢。
    return now_ms >= then_ms ? now_ms - then_ms : 0;
}

// 使用服务器墙上时间判断网络状态是否缺失、来自未来或超过新鲜度阈值。
bool wallStateStale(uint64_t received_at_ms, uint64_t wall_now_ms, uint64_t stale_ms) {
    return received_at_ms == 0 || received_at_ms > wall_now_ms ||
           elapsedSince(wall_now_ms, received_at_ms) > stale_ms;
}

// 创建带共线资源公共字段的基础决策，减少各个提前返回分支的重复代码。
DispatchDecision makeDecision(DispatchAction action,
                              const SharedCorridorState& corridor,
                              const std::string& reason) {
    DispatchDecision decision;
    decision.action = action;
    decision.corridor_resource_id = corridor.corridor_resource_id;
    decision.corridor_id = corridor.corridor_id;
    decision.direction = corridor.direction;
    decision.reason = reason;
    return decision;
}
} // namespace

// 校验时间阈值和各评分权重，防止 NaN/Inf 破坏候选排序的确定性。
DynamicPriorityPolicy::DynamicPriorityPolicy(DynamicPriorityConfig config)
    : config_(config) {
    if (config_.default_corridor_minimum_headway_ms == 0 || config_.maximum_wait_ms == 0) {
        throw std::invalid_argument("dispatch time limits must be greater than zero");
    }
    if (!std::isfinite(config_.headway_urgency_weight) ||
        !std::isfinite(config_.waiting_time_weight) ||
        !std::isfinite(config_.fairness_credit_weight) ||
        !std::isfinite(config_.global_plan_weight) ||
        !std::isfinite(config_.consecutive_penalty_weight)) {
        throw std::invalid_argument("dispatch weights must be finite");
    }
}

// 返回策略的只读配置引用，不复制权重和阈值。
const DynamicPriorityConfig& DynamicPriorityPolicy::config() const { return config_; }

// 先执行不可突破的安全硬约束，再计算软分数、防饥饿和稳定的平局规则。
DispatchDecision DynamicPriorityPolicy::decide(const DispatchContext& context) const {
    const SharedCorridorState& corridor = context.corridor;
    const uint64_t simulation_now_ms = context.simulation_now_ms;
    const uint64_t wall_now_ms = context.wall_now_ms;

    // 先处理不可突破的硬约束。动态分数再高，也不能绕过这些条件。
    if (wallStateStale(corridor.received_at_wall_ms, wall_now_ms,
                       config_.telemetry_stale_wall_ms)) {
        return makeDecision(DispatchAction::HOLD_ALL, corridor, "corridor_state_stale");
    }
    if (corridor.command_pending) {
        return makeDecision(DispatchAction::NO_ACTION, corridor, "command_pending");
    }
    if (!corridor.downstream_available) {
        return makeDecision(DispatchAction::HOLD_ALL, corridor, "downstream_unavailable");
    }
    if (corridor.capacity == 0 || corridor.occupancy >= corridor.capacity) {
        return makeDecision(DispatchAction::HOLD_ALL, corridor, "corridor_at_capacity");
    }

    const uint64_t minimum_headway_ms = corridor.minimum_headway_ms == 0
        ? config_.default_corridor_minimum_headway_ms : corridor.minimum_headway_ms;
    if (corridor.last_entry_simulation_ms != 0 &&
        elapsedSince(simulation_now_ms, corridor.last_entry_simulation_ms) < minimum_headway_ms) {
        return makeDecision(DispatchAction::HOLD_ALL, corridor, "minimum_headway_not_met");
    }

    struct ScoredCandidate {
        const DispatchCandidate* candidate = nullptr;
        DispatchScoreBreakdown breakdown;
        double score = 0.0;
        uint64_t waiting_ms = 0;
        bool maximum_wait_exceeded = false;
        bool consecutive_limit_reached = false;
    };

    std::vector<ScoredCandidate> scored;
    scored.reserve(corridor.candidates.size());
    for (const DispatchCandidate& candidate : corridor.candidates) {
        // 路线、方向、资源或时间不一致的状态不进入本轮评分。
        if (candidate.train_id.empty() || candidate.line_id.empty() ||
            candidate.corridor_resource_id != corridor.corridor_resource_id ||
            candidate.corridor_direction != corridor.direction ||
            candidate.arrived_at_simulation_ms == 0 ||
            candidate.arrived_at_simulation_ms > simulation_now_ms ||
            wallStateStale(candidate.received_at_wall_ms, wall_now_ms,
                           config_.telemetry_stale_wall_ms) ||
            !std::isfinite(candidate.global_plan_priority)) {
            continue;
        }
        auto line_it = context.lines.find(candidate.line_id);
        if (line_it == context.lines.end() || line_it->second.target_headway_ms == 0) continue;

        const LineDispatchState& line = line_it->second;
        ScoredCandidate item;
        item.candidate = &candidate;
        item.waiting_ms = elapsedSince(simulation_now_ms,
                                       candidate.arrived_at_simulation_ms);
        item.maximum_wait_exceeded = item.waiting_ms >= config_.maximum_wait_ms;
        item.consecutive_limit_reached = config_.max_consecutive_release > 0 &&
            candidate.line_id == context.last_released_line_id &&
            context.consecutive_release_count >= config_.max_consecutive_release;

        // 首次出现的线路按“刚好达到一个目标间隔”初始化，避免异常高分。
        const uint64_t since_release = line.last_release_simulation_ms == 0
            ? line.target_headway_ms
            : elapsedSince(simulation_now_ms, line.last_release_simulation_ms);
        item.breakdown.headway_urgency = static_cast<double>(since_release) /
                                          static_cast<double>(line.target_headway_ms);
        item.breakdown.waiting_time = static_cast<double>(item.waiting_ms) /
                                      static_cast<double>(config_.maximum_wait_ms);
        item.breakdown.fairness_credit = line.fairness_credit;
        item.breakdown.global_plan_priority = candidate.global_plan_priority;
        item.breakdown.consecutive_penalty =
            candidate.line_id == context.last_released_line_id
                ? static_cast<double>(context.consecutive_release_count) : 0.0;
        // 分数越高越优先：班次缺口、等待、公平信用和全线滚动建议加分，
        // 连续放行扣分。这里只融合软分数，前面的安全硬约束仍不可突破。
        item.score = config_.headway_urgency_weight * item.breakdown.headway_urgency +
                     config_.waiting_time_weight * item.breakdown.waiting_time +
                     config_.fairness_credit_weight * item.breakdown.fairness_credit +
                     config_.global_plan_weight * item.breakdown.global_plan_priority -
                     config_.consecutive_penalty_weight * item.breakdown.consecutive_penalty;
        scored.push_back(item);
    }

    if (scored.empty()) {
        return makeDecision(DispatchAction::NO_ACTION, corridor, "no_feasible_candidate");
    }
    const bool has_alternative_line = std::any_of(
        scored.begin(), scored.end(), [&](const ScoredCandidate& item) {
            return item.candidate->line_id != context.last_released_line_id;
        });
    auto eligible = [&](const ScoredCandidate& item) {
        return item.maximum_wait_exceeded ||
               !(item.consecutive_limit_reached && has_alternative_line);
    };

    const ScoredCandidate* selected = nullptr;
    // 防饥饿具有高于软评分的优先级，但仍然不能突破前面的安全硬约束。
    for (const ScoredCandidate& item : scored) {
        if (!eligible(item) || !item.maximum_wait_exceeded) continue;
        if (!selected || item.waiting_ms > selected->waiting_ms ||
            (item.waiting_ms == selected->waiting_ms && item.score > selected->score)) {
            selected = &item;
        }
    }
    if (!selected) {
        // 正常模式按动态分数选择；等待时间和 train_id 保证稳定、可复现。
        for (const ScoredCandidate& item : scored) {
            if (!eligible(item)) continue;
            if (!selected || item.score > selected->score ||
                (item.score == selected->score && item.waiting_ms > selected->waiting_ms) ||
                (item.score == selected->score && item.waiting_ms == selected->waiting_ms &&
                 item.candidate->train_id < selected->candidate->train_id)) {
                selected = &item;
            }
        }
    }
    if (!selected) {
        return makeDecision(DispatchAction::NO_ACTION, corridor, "no_eligible_candidate");
    }

    DispatchDecision decision = makeDecision(
        DispatchAction::RELEASE, corridor,
        selected->maximum_wait_exceeded ? "maximum_wait_exceeded"
                                        : "highest_dynamic_priority");
    decision.train_id = selected->candidate->train_id;
    decision.line_id = selected->candidate->line_id;
    decision.reserved_intersection_movement_ids =
        selected->candidate->required_intersection_movement_ids;
    decision.score = selected->score;
    decision.breakdown = selected->breakdown;
    return decision;
}
