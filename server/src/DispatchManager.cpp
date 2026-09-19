#include "DispatchManager.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

// 组装动态策略和预约器，并验证会影响资源安全的默认配置。
DispatchManager::DispatchManager(DispatchManagerConfig config)
    : policy_(config.priority),
      reservations_(config.default_intersection_clearance_ms),
      default_corridor_capacity_(config.default_corridor_capacity) {
    if (config.default_intersection_clearance_ms == 0 ||
        default_corridor_capacity_ == 0) {
        throw std::invalid_argument("dispatch resource defaults must be greater than zero");
    }
}

// 将拓扑分析生成的 movement 转交给全局交叉点预约器。
bool DispatchManager::registerIntersectionMovement(
    const LineIntersectionMovement& movement) {
    return reservations_.registerMovement(movement);
}

// 新增或刷新共线方向资源；外部快照不得覆盖服务器内部待 ACK 状态。
void DispatchManager::upsertCorridor(const SharedCorridorState& corridor) {
    if (corridor.corridor_resource_id.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    SharedCorridorState updated = corridor;
    auto existing = corridors_.find(corridor.corridor_resource_id);
    if (existing != corridors_.end()) {
        // 外部快照不能覆盖服务器正在等待确认的命令状态；进入时间只前进。
        updated.command_pending = existing->second.command_pending;
        updated.last_entry_simulation_ms = std::max(
            updated.last_entry_simulation_ms,
            existing->second.last_entry_simulation_ms);
    }
    if (updated.minimum_headway_ms == 0) {
        updated.minimum_headway_ms =
            policy_.config().default_corridor_minimum_headway_ms;
    }
    if (updated.capacity == 0) updated.capacity = default_corridor_capacity_;
    corridors_[corridor.corridor_resource_id] = std::move(updated);
}

// 保存某条线路在指定资源下的班次和公平状态，非法主键或零间隔直接忽略。
void DispatchManager::upsertLineState(const std::string& corridor_resource_id,
                                      const std::string& line_id,
                                      const LineDispatchState& state) {
    if (corridor_resource_id.empty() || line_id.empty() ||
        state.target_headway_ms == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    line_states_[corridor_resource_id][line_id] = state;
}

// 仅刷新一轮遥测派生候选和墙上时间，不重置 occupancy、进入时间或待命令。
bool DispatchManager::replaceCandidates(
    const std::string& corridor_resource_id,
    std::vector<DispatchCandidate> candidates,
    uint64_t received_at_wall_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto corridor_it = corridors_.find(corridor_resource_id);
    if (corridor_it == corridors_.end()) return false;
    corridor_it->second.candidates = std::move(candidates);
    corridor_it->second.received_at_wall_ms = received_at_wall_ms;
    return true;
}

// 按经过的目标班次数累计信用并限制上限；调用点已持有管理器互斥锁。
void DispatchManager::updateFairnessCreditsLocked(
    const std::string& corridor_resource_id,
    uint64_t simulation_now_ms) {
    auto lines_it = line_states_.find(corridor_resource_id);
    if (lines_it == line_states_.end()) return;
    for (auto& pair : lines_it->second) {
        LineDispatchState& line = pair.second;
        if (line.last_credit_update_simulation_ms == 0 ||
            simulation_now_ms < line.last_credit_update_simulation_ms) {
            line.last_credit_update_simulation_ms = simulation_now_ms;
            continue;
        }
        const uint64_t elapsed = simulation_now_ms -
                                 line.last_credit_update_simulation_ms;
        // 每经过一个目标班次间隔积累 1 份信用，上限防止长期离线后暴涨。
        line.fairness_credit = std::min(
            3.0,
            line.fairness_credit + static_cast<double>(elapsed) /
                                   static_cast<double>(line.target_headway_ms));
        line.last_credit_update_simulation_ms = simulation_now_ms;
    }
}

// 对一个共线方向执行完整决策：更新公平性、预过滤交叉冲突、调用策略，
// 最后原子预约交叉 movement 并记录待 ACK 命令。
DispatchDecision DispatchManager::planNext(
                                           const std::string& corridor_resource_id,
                                           uint64_t simulation_now_ms,
                                           uint64_t wall_now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto corridor_it = corridors_.find(corridor_resource_id);
    if (corridor_it == corridors_.end()) {
        DispatchDecision decision;
        decision.reason = "unknown_shared_corridor_resource";
        return decision;
    }

    updateFairnessCreditsLocked(corridor_resource_id, simulation_now_ms);
    DispatchContext context;
    context.corridor = corridor_it->second;
    context.lines = line_states_[corridor_resource_id];
    context.last_released_line_id = last_released_line_[corridor_resource_id];
    context.consecutive_release_count =
        consecutive_release_count_[corridor_resource_id];
    context.simulation_now_ms = simulation_now_ms;
    context.wall_now_ms = wall_now_ms;

    // FORWARD/REVERSE 是同一段实体轨道的两个行驶方向。任一方向已有列车或
    // 待确认 RELEASE 时，另一方向必须保持关闭，避免把方向资源误当成两条轨道。
    for (const auto& pair : corridors_) {
        if (pair.first == corridor_resource_id ||
            pair.second.corridor_id != corridor_it->second.corridor_id) continue;
        if (pair.second.occupancy > 0 || pair.second.command_pending) {
            DispatchDecision decision;
            decision.action = corridor_it->second.candidates.empty()
                ? DispatchAction::NO_ACTION : DispatchAction::HOLD_ALL;
            decision.corridor_resource_id = corridor_resource_id;
            decision.corridor_id = corridor_it->second.corridor_id;
            decision.direction = corridor_it->second.direction;
            decision.reason = pair.second.occupancy > 0
                ? "opposing_direction_occupied" : "opposing_direction_pending";
            return decision;
        }
    }

    const bool had_candidates = !context.corridor.candidates.empty();
    std::string intersection_reason;
    // 先用交叉点硬约束过滤，再把安全候选交给动态优先级策略。
    context.corridor.candidates.erase(
        std::remove_if(context.corridor.candidates.begin(),
                       context.corridor.candidates.end(),
            [&](const DispatchCandidate& candidate) {
                std::string reason;
                if (reservations_.canReserve(
                        candidate.required_intersection_movement_ids,
                                             simulation_now_ms, &reason)) {
                    return false;
                }
                intersection_reason = reason;
                return true;
            }),
        context.corridor.candidates.end());

    if (had_candidates && context.corridor.candidates.empty()) {
        DispatchDecision decision;
        decision.action = DispatchAction::HOLD_ALL;
        decision.corridor_resource_id = corridor_it->second.corridor_resource_id;
        decision.corridor_id = corridor_it->second.corridor_id;
        decision.direction = corridor_it->second.direction;
        decision.reason = intersection_reason.empty()
            ? "line_intersection_conflict" : intersection_reason;
        return decision;
    }

    DispatchDecision decision = policy_.decide(context);
    if (decision.action != DispatchAction::RELEASE) return decision;

    std::string reserve_reason;
    // canReserve 与实际写入之间可能有其他方向抢占资源，因此必须在
    // 策略选出列车后再次原子校验并预约。
    if (!reservations_.tryReserveAtomically(
            decision.train_id, decision.reserved_intersection_movement_ids,
            simulation_now_ms, &reserve_reason)) {
        decision.action = DispatchAction::HOLD_ALL;
        decision.reason = reserve_reason.empty()
            ? "line_intersection_reservation_failed" : reserve_reason;
        decision.train_id.clear();
        decision.line_id.clear();
        decision.reserved_intersection_movement_ids.clear();
        return decision;
    }

    // 预约成功只代表可以下发命令；收到游戏 ACK 前不修改实际进入时间。
    corridor_it->second.command_pending = true;
    pending_decisions_[corridor_resource_id] = decision;
    return decision;
}

// 只有 ACK 的资源和 train_id 与待命令完全匹配时，才确认进入并更新容量、
// 上次放行、信用和连续放行历史。
bool DispatchManager::confirmEntry(const std::string& corridor_resource_id,
                                   const std::string& train_id,
                                   uint64_t simulation_now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto pending_it = pending_decisions_.find(corridor_resource_id);
    auto corridor_it = corridors_.find(corridor_resource_id);
    if (pending_it == pending_decisions_.end() ||
        corridor_it == corridors_.end() ||
        pending_it->second.train_id != train_id) {
        return false;
    }

    const DispatchDecision& decision = pending_it->second;
    // 只有游戏确认已经进入，才正式消耗共线段容量并更新线路历史。
    SharedCorridorState& corridor = corridor_it->second;
    corridor.command_pending = false;
    corridor.last_entry_simulation_ms = simulation_now_ms;
    if (corridor.occupancy < corridor.capacity) ++corridor.occupancy;
    corridor.candidates.erase(
        std::remove_if(corridor.candidates.begin(), corridor.candidates.end(),
            [&](const DispatchCandidate& candidate) {
                return candidate.train_id == train_id;
            }),
        corridor.candidates.end());

    LineDispatchState& line =
        line_states_[corridor_resource_id][decision.line_id];
    line.last_release_simulation_ms = simulation_now_ms;
    line.last_credit_update_simulation_ms = simulation_now_ms;
    line.fairness_credit = std::max(0.0, line.fairness_credit - 1.0);

    if (last_released_line_[corridor_resource_id] == decision.line_id) {
        ++consecutive_release_count_[corridor_resource_id];
    } else {
        last_released_line_[corridor_resource_id] = decision.line_id;
        consecutive_release_count_[corridor_resource_id] = 1;
    }
    pending_decisions_.erase(pending_it);
    return true;
}

// 取消尚未执行的 RELEASE，撤销交叉点预预约并恢复资源可决策状态。
bool DispatchManager::cancelPending(const std::string& corridor_resource_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto pending_it = pending_decisions_.find(corridor_resource_id);
    if (pending_it == pending_decisions_.end()) return false;
    // 命令执行失败或超时时释放预占资源，但不产生 2 秒清空时间，
    // 因为列车实际上没有进入交叉点。
    reservations_.cancelTrain(pending_it->second.train_id);
    auto corridor_it = corridors_.find(corridor_resource_id);
    if (corridor_it != corridors_.end())
        corridor_it->second.command_pending = false;
    pending_decisions_.erase(pending_it);
    return true;
}

// 恢复待确认命令时重新执行交叉点原子预约，避免数据库中的互相冲突状态
// 在内存中被静默接受。
bool DispatchManager::restorePending(const DispatchDecision& decision,
                                     std::string* reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto corridor = corridors_.find(decision.corridor_resource_id);
    if (corridor == corridors_.end() || corridor->second.command_pending) {
        if (reason) *reason = "unknown_or_busy_corridor";
        return false;
    }
    for (const auto& pair : corridors_) {
        if (pair.first != decision.corridor_resource_id &&
            pair.second.corridor_id == corridor->second.corridor_id &&
            (pair.second.occupancy > 0 || pair.second.command_pending)) {
            if (reason) *reason = "opposing_direction_busy";
            return false;
        }
    }
    if (!reservations_.tryReserveAtomically(
            decision.train_id,
            decision.reserved_intersection_movement_ids,
            decision.issued_at_simulation_ms, reason)) return false;
    corridor->second.command_pending = true;
    pending_decisions_[decision.corridor_resource_id] = decision;
    return true;
}

// 已接受命令代表列车可能仍在受控资源内，因此恢复时必须保守地占用容量；
// 只有后续明确的 cleared 事件才能释放它。
bool DispatchManager::restoreActive(const DispatchDecision& decision,
                                    std::string* reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto corridor = corridors_.find(decision.corridor_resource_id);
    if (corridor == corridors_.end()) {
        if (reason) *reason = "unknown_corridor";
        return false;
    }
    for (const auto& pair : corridors_) {
        if (pair.first != decision.corridor_resource_id &&
            pair.second.corridor_id == corridor->second.corridor_id &&
            (pair.second.occupancy > 0 || pair.second.command_pending)) {
            if (reason) *reason = "opposing_direction_busy";
            return false;
        }
    }
    if (!reservations_.tryReserveAtomically(
            decision.train_id,
            decision.reserved_intersection_movement_ids,
            decision.issued_at_simulation_ms, reason)) return false;
    if (corridor->second.occupancy < corridor->second.capacity)
        ++corridor->second.occupancy;
    corridor->second.last_entry_simulation_ms = std::max(
        corridor->second.last_entry_simulation_ms,
        decision.issued_at_simulation_ms);
    return true;
}

// 车尾离开共线段后安全减少占用；未知资源或零占用返回 false。
bool DispatchManager::markCorridorExited(
    const std::string& corridor_resource_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto corridor_it = corridors_.find(corridor_resource_id);
    if (corridor_it == corridors_.end() ||
        corridor_it->second.occupancy == 0) return false;
    --corridor_it->second.occupancy;
    return true;
}

// 转发交叉点清空事件；预约器负责记录模拟时间和后续 2 秒硬间隔。
bool DispatchManager::markIntersectionCleared(
                                          const std::string& intersection_id,
                                          const std::string& train_id,
                                          uint64_t simulation_now_ms) {
    return reservations_.markCleared(
        intersection_id, train_id, simulation_now_ms);
}

// 线程安全返回指定共线方向资源的状态副本。
std::optional<SharedCorridorState> DispatchManager::corridorState(
    const std::string& corridor_resource_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = corridors_.find(corridor_resource_id);
    if (it == corridors_.end()) return std::nullopt;
    return it->second;
}

std::vector<IntersectionReservation>
// 返回预约器中的全部活动交叉 movement，不额外持有 DispatchManager 的锁。
DispatchManager::activeIntersectionReservations() const {
    return reservations_.activeReservations();
}
